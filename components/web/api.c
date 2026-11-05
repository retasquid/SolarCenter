#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "app_state.h"
#include "config.h"
#include "net.h"
#include "sysmon.h"
#include "jwt_exp.h"
#include "forecast.h"
#include "web_priv.h"

static const char *TAG = "web";

#define BODY_MAX                4096
#define HEAP_WARN_BYTES         (40 * 1024)
#define NTP_LOST_ALERT_S        (24 * 3600)
#define CHIP_REV_SECURE_BOOT_V2 300
#define SETUP_REBOOT_DELAY_US   (1500 * 1000)

/* Tampons statiques : les gestionnaires s'exécutent tous dans l'unique tâche httpd. */
static char s_body[BODY_MAX];
static cfg_t s_cfg;
static esp_timer_handle_t s_reboot_timer;

/* ---------- GET /api/system ---------- */

static esp_err_t system_get(httpd_req_t *req)
{
    app_state_t st;
    app_state_get(&st);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) != ESP_OK) {
        flash = 0;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    time_t now = time(NULL);

    cJSON *r = cJSON_CreateObject();
    if (r == NULL) {
        return web_send_json(req, NULL);
    }
    cJSON_AddStringToObject(r, "version", app->version);
    cJSON_AddStringToObject(r, "idf", app->idf_ver);
    cJSON_AddNumberToObject(r, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(r, "reset_reason", (int)esp_reset_reason());

    cJSON_AddNumberToObject(r, "heap_free", st.heap_free);
    cJSON_AddNumberToObject(r, "heap_min", st.heap_min);
    cJSON_AddNumberToObject(r, "heap_warn", HEAP_WARN_BYTES);

    cJSON_AddBoolToObject(r, "wifi_connected", st.wifi_connected);
    cJSON_AddBoolToObject(r, "ap_active", st.ap_active);
    cJSON_AddBoolToObject(r, "setup_mode", net_is_setup_mode());
    cJSON_AddNumberToObject(r, "rssi", st.rssi);
    cJSON_AddNumberToObject(r, "wifi_disconnects", st.wifi_disconnects);
    cJSON_AddNumberToObject(r, "wifi_last_reason", st.wifi_last_reason);
    cJSON_AddStringToObject(r, "ip", st.ip);

    char rev[8];
    snprintf(rev, sizeof(rev), "v%d.%d", chip.revision / 100, chip.revision % 100);
    cJSON_AddStringToObject(r, "chip_rev", rev);
    cJSON_AddBoolToObject(r, "secure_boot_v2", chip.revision >= CHIP_REV_SECURE_BOOT_V2);
    cJSON_AddNumberToObject(r, "flash_size", flash);

    cJSON_AddBoolToObject(r, "time_synced", st.time_synced);
    cJSON_AddNumberToObject(r, "last_ntp_sync", (double)st.last_ntp_sync);
    cJSON_AddBoolToObject(r, "ntp_alert", !st.time_synced || now - st.last_ntp_sync > NTP_LOST_ALERT_S);
    if (st.time_synced) {
        struct tm tm;
        char buf[24];
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        cJSON_AddStringToObject(r, "time", buf);
    }

    config_get(&s_cfg);
    cJSON_AddStringToObject(r, "cfg_status", config_status_str(config_status()));
    cJSON_AddBoolToObject(r, "admin_set", s_cfg.sys.admin_pass[0] != '\0');
    cJSON_AddBoolToObject(r, "tariff_configured", s_cfg.tariff.configured);
    cJSON_AddBoolToObject(r, "observer", s_cfg.sys.observer);
    cJSON_AddStringToObject(r, "hostname", s_cfg.sys.hostname);
    cJSON_AddNumberToObject(r, "envoy_token_exp", (double)jwt_exp(s_cfg.dev.envoy_token));
    cJSON_AddBoolToObject(r, "envoy_skip_cert", s_cfg.dev.envoy_fw7 && s_cfg.dev.envoy_skip_cert);
    return web_send_json(req, r);
}

/* ---------- GET /api/state ---------- */

static esp_err_t state_get(httpd_req_t *req)
{
    return web_send_json(req, state_to_json());
}

/* ---------- GET / POST /api/config ---------- */

static esp_err_t config_get_handler(httpd_req_t *req)
{
    config_get(&s_cfg);
    return web_send_json(req, cfg_to_json(&s_cfg));
}

/* Lit et analyse le corps JSON ; en cas d'échec la réponse d'erreur est déjà envoyée. */
static cJSON *read_json(httpd_req_t *req, esp_err_t *err)
{
    int n = web_read_body(req, s_body, sizeof(s_body));
    if (n == -1) {
        *err = web_send_error(req, WEB_413, "corps trop volumineux", NULL);
        return NULL;
    }
    if (n < 0) {
        *err = web_send_error(req, WEB_400, "lecture du corps impossible", NULL);
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(s_body, (size_t)n);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *err = web_send_error(req, WEB_400, "JSON invalide", NULL);
        return NULL;
    }
    return root;
}

static esp_err_t save_and_reply(httpd_req_t *req, bool reboot_required, bool *saved)
{
    const char *bad = NULL;
    esp_err_t err = config_save(&s_cfg, &bad);
    *saved = (err == ESP_OK);
    if (err == ESP_ERR_INVALID_ARG) {
        return web_send_error(req, WEB_400, "valeur invalide", bad);
    }
    if (err != ESP_OK) {
        return web_send_error(req, WEB_500, "enregistrement impossible", NULL);
    }
    cJSON *r = cJSON_CreateObject();
    if (r != NULL) {
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddBoolToObject(r, "reboot_required", reboot_required);
    }
    return web_send_json(req, r);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!config_admin_is_set()) {
        return web_send_error(req, WEB_403, "mot de passe administrateur non défini, passer par /setup",
                              "sys.admin_pass");
    }
    if (!web_check_admin(req)) {
        return web_send_error(req, WEB_401, "mot de passe administrateur invalide", NULL);
    }
    esp_err_t err = ESP_OK;
    cJSON *root = read_json(req, &err);
    if (root == NULL) {
        return err;
    }

    config_get(&s_cfg);
    cfg_sys_t old_sys;
    memcpy(&old_sys, &s_cfg.sys, sizeof(old_sys));
    cfg_pv_t old_pv = s_cfg.pv;
    const char *bad = cfg_from_json(root, &s_cfg);
    cJSON_Delete(root);
    if (bad != NULL) {
        return web_send_error(req, WEB_400, "valeur invalide", bad);
    }

    bool reboot = strcmp(old_sys.wifi_ssid, s_cfg.sys.wifi_ssid) != 0 ||
                  strcmp(old_sys.wifi_pass, s_cfg.sys.wifi_pass) != 0 ||
                  strcmp(old_sys.hostname, s_cfg.sys.hostname) != 0;
    memset(&old_sys, 0, sizeof(old_sys));
    bool saved;
    err = save_and_reply(req, reboot, &saved);
    if (saved && memcmp(&old_pv, &s_cfg.pv, sizeof(old_pv)) != 0) {
        forecast_refresh();     /* coordonnées ou orientation modifiées */
    }
    if (saved && reboot) {
        ESP_LOGI(TAG, "WiFi ou nom réseau modifié : redémarrage");
        if (esp_timer_start_once(s_reboot_timer, SETUP_REBOOT_DELAY_US) != ESP_OK) {
            ESP_LOGE(TAG, "minuterie de redémarrage indisponible");
        }
    }
    return err;
}

/* ---------- POST /api/setup (portail) ---------- */

static void on_reboot_timer(void *arg)
{
    sysmon_restart("nouvelle configuration réseau");
}

/* Corps : {"wifi_ssid","wifi_pass","admin_pass","hostname"}. Sans mot de passe au premier
 * démarrage, ensuite le mot de passe administrateur actuel est exigé. */
static esp_err_t setup_post_handler(httpd_req_t *req)
{
    bool first = !config_admin_is_set();
    if (!first && !web_check_admin(req)) {
        return web_send_error(req, WEB_401, "mot de passe administrateur invalide", NULL);
    }
    esp_err_t err = ESP_OK;
    cJSON *root = read_json(req, &err);
    if (root == NULL) {
        return err;
    }

    config_get(&s_cfg);
    cfg_sys_t *s = &s_cfg.sys;
    const char *bad = NULL;
    if (!json_get_str(root, "wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid), false)) {
        bad = "sys.wifi_ssid";
    } else if (!json_get_str(root, "wifi_pass", s->wifi_pass, sizeof(s->wifi_pass), true)) {
        bad = "sys.wifi_pass";
    } else if (!json_get_str(root, "admin_pass", s->admin_pass, sizeof(s->admin_pass), true)) {
        bad = "sys.admin_pass";
    } else if (!json_get_str(root, "hostname", s->hostname, sizeof(s->hostname), false)) {
        bad = "sys.hostname";
    } else if (s->wifi_ssid[0] == '\0') {
        bad = "sys.wifi_ssid";
    } else if (s->admin_pass[0] == '\0') {
        bad = "sys.admin_pass";
    }
    cJSON_Delete(root);
    if (bad != NULL) {
        return web_send_error(req, WEB_400, "valeur invalide", bad);
    }

    bool saved;
    err = save_and_reply(req, true, &saved);
    if (saved) {
        ESP_LOGI(TAG, "portail : configuration enregistrée, redémarrage");
        if (esp_timer_start_once(s_reboot_timer, SETUP_REBOOT_DELAY_US) != ESP_OK) {
            ESP_LOGE(TAG, "minuterie de redémarrage indisponible");
        }
    }
    return err;
}

esp_err_t api_register(httpd_handle_t srv)
{
    const esp_timer_create_args_t targs = { .callback = on_reboot_timer, .name = "setup_reboot" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_reboot_timer), TAG, "timer");

    const httpd_uri_t routes[] = {
        { .uri = "/api/system", .method = HTTP_GET,  .handler = system_get },
        { .uri = "/api/state",  .method = HTTP_GET,  .handler = state_get },
        { .uri = "/api/config", .method = HTTP_GET,  .handler = config_get_handler },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/setup",  .method = HTTP_POST, .handler = setup_post_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(srv, &routes[i]), TAG, "%s", routes[i].uri);
    }
    ESP_RETURN_ON_ERROR(api_hist_register(srv), TAG, "historique");
    return api_ctl_register(srv);
}
