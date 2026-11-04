#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "mdns.h"
#include "app_state.h"
#include "net.h"

static const char *TAG = "net";

#define NET_SETUP_AP_SSID       "Solaire-Setup"
#define NET_SETUP_AP_CHANNEL    1
#define NET_SETUP_AP_MAX_CONN   2
#define NET_TZ                  "CET-1CEST,M3.5.0,M10.5.0/3"
#define NET_NTP_SERVER          "pool.ntp.org"
#define NET_MDNS_INSTANCE       "Gestionnaire solaire"
#define NET_FALLBACK_AP_AFTER   5       /* échecs STA consécutifs avant d'ouvrir le portail de secours */
#define NET_RETRY_MIN_MS        1000
#define NET_RETRY_MAX_MS        30000

static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_ms = NET_RETRY_MIN_MS;
static uint32_t s_fail_count;
static bool s_setup_mode;       /* aucun réseau configuré : portail seul */
static bool s_ap_fallback;      /* portail ouvert en secours à côté de la STA */
static bool s_sntp_started;

static void set_ap_state(bool active)
{
    app_state_t *st = app_state_lock();
    st->ap_active = active;
    app_state_unlock();
}

static esp_err_t ap_config_apply(void)
{
    wifi_config_t ap = { 0 };
    memcpy(ap.ap.ssid, NET_SETUP_AP_SSID, strlen(NET_SETUP_AP_SSID));
    ap.ap.ssid_len = strlen(NET_SETUP_AP_SSID);
    ap.ap.channel = NET_SETUP_AP_CHANNEL;
    ap.ap.max_connection = NET_SETUP_AP_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_AP, &ap);
}

static void start_fallback_ap(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = ap_config_apply();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "portail de secours impossible : %s", esp_err_to_name(err));
        return;
    }
    s_ap_fallback = true;
    set_ap_state(true);
    ESP_LOGW(TAG, "WiFi injoignable : portail de secours « %s » sur http://192.168.4.1", NET_SETUP_AP_SSID);
}

static void on_time_sync(struct timeval *tv)
{
    app_state_t *st = app_state_lock();
    st->time_synced = true;
    st->last_ntp_sync = tv->tv_sec;
    app_state_unlock();

    struct tm tm;
    char buf[32];
    localtime_r(&tv->tv_sec, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "heure synchronisée : %s", buf);
}

static void start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG(NET_NTP_SERVER);
    c.sync_cb = on_time_sync;
    esp_err_t err = esp_netif_sntp_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP : %s", esp_err_to_name(err));
        return;
    }
    s_sntp_started = true;
}

static void on_retry_timer(void *arg)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect : %s", esp_err_to_name(err));
    }
}

static void schedule_retry(void)
{
    esp_timer_stop(s_retry_timer);  /* ESP_ERR_INVALID_STATE si déjà arrêté : sans effet */
    esp_err_t err = esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "minuterie de reconnexion : %s", esp_err_to_name(err));
    }
    s_retry_ms = (s_retry_ms * 2 > NET_RETRY_MAX_MS) ? NET_RETRY_MAX_MS : s_retry_ms * 2;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        on_retry_timer(NULL);
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        s_fail_count++;
        app_state_t *st = app_state_lock();
        st->wifi_connected = false;
        st->ip[0] = '\0';
        st->rssi = 0;
        st->wifi_disconnects++;
        st->wifi_last_reason = (uint8_t)ev->reason;
        app_state_unlock();
        ESP_LOGW(TAG, "WiFi déconnecté (raison %d), nouvel essai dans %lu ms",
                 ev->reason, (unsigned long)s_retry_ms);
        if (s_fail_count == NET_FALLBACK_AP_AFTER && !s_ap_fallback) {
            start_fallback_ap();
        }
        schedule_retry();
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "client connecté au portail");
        break;

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *ev = data;
    s_fail_count = 0;
    s_retry_ms = NET_RETRY_MIN_MS;

    app_state_t *st = app_state_lock();
    st->wifi_connected = true;
    snprintf(st->ip, sizeof(st->ip), IPSTR, IP2STR(&ev->ip_info.ip));
    app_state_unlock();
    ESP_LOGI(TAG, "connecté, IP " IPSTR, IP2STR(&ev->ip_info.ip));

    if (s_ap_fallback) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            s_ap_fallback = false;
            set_ap_state(false);
            ESP_LOGI(TAG, "portail de secours fermé");
        } else {
            ESP_LOGW(TAG, "fermeture du portail : %s", esp_err_to_name(err));
        }
    }
    start_sntp();
}

static void mdns_start(const char *hostname)
{
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        err = mdns_hostname_set(hostname);
    }
    if (err == ESP_OK) {
        err = mdns_instance_name_set(NET_MDNS_INSTANCE);
    }
    if (err == ESP_OK) {
        err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS indisponible : %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "mDNS : http://%s.local", hostname);
}

esp_err_t net_start(const cfg_t *cfg)
{
    setenv("TZ", NET_TZ, 1);
    tzset();

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(sta != NULL && ap != NULL, ESP_FAIL, TAG, "création des netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(sta, cfg->sys.hostname), TAG, "hostname");

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "esp_wifi_init");
    /* Les identifiants vivent dans notre config, pas dans la NVS du pilote WiFi. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            on_wifi_event, NULL, NULL), TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_ip_event, NULL, NULL), TAG, "ip evt");

    const esp_timer_create_args_t targs = { .callback = on_retry_timer, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry_timer), TAG, "timer");

    if (cfg->sys.wifi_ssid[0] == '\0') {
        s_setup_mode = true;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "mode AP");
        ESP_RETURN_ON_ERROR(ap_config_apply(), TAG, "config AP");
        set_ap_state(true);
        ESP_LOGW(TAG, "aucun réseau WiFi configuré : portail « %s » sur http://192.168.4.1",
                 NET_SETUP_AP_SSID);
    } else {
        wifi_config_t sta_cfg = { 0 };
        memcpy(sta_cfg.sta.ssid, cfg->sys.wifi_ssid,
               strnlen(cfg->sys.wifi_ssid, sizeof(sta_cfg.sta.ssid)));
        memcpy(sta_cfg.sta.password, cfg->sys.wifi_pass,
               strnlen(cfg->sys.wifi_pass, sizeof(sta_cfg.sta.password)));
        sta_cfg.sta.threshold.authmode = cfg->sys.wifi_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode STA");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "config STA");
        /* 20 MHz : plus robuste que 40 MHz dans une bande 2,4 GHz encombrée. */
        esp_err_t bw_err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
        if (bw_err != ESP_OK) {
            ESP_LOGW(TAG, "largeur de canal : %s", esp_err_to_name(bw_err));
        }
        ESP_LOGI(TAG, "connexion à « %s »", cfg->sys.wifi_ssid);
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    /* Alimenté sur secteur : pas d'économie d'énergie radio, qui ajoute latence et pertes
     * (constaté avec un signal faible). */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "économie d'énergie WiFi : %s", esp_err_to_name(ps_err));
    }
    mdns_start(cfg->sys.hostname);
    return ESP_OK;
}

bool net_is_setup_mode(void)
{
    return s_setup_mode;
}

int8_t net_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}
