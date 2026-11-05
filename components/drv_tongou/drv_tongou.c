#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cJSON.h"
#include "http_util.h"
#include "drv_tuya.h"
#include "drv_tongou.h"

static const char *TAG = "drv_tongou";

/* TODO(verify) : commande exacte et chemin JSON selon la version d'OpenBeken (§8.4). */
#define TONGOU_URL_ENERGY   "http://%s/cm?cmnd=Status%%2010"
#define TK_STATUS_SNS       "StatusSNS"
#define TK_ENERGY           "ENERGY"
#define TK_POWER            "Power"

/* TODO(verify) : état du relais par la commande Tasmota « POWER » sans argument,
 * réponse supposée {"POWER":"ON"|"OFF"}. Absente du CLAUDE.md, à confirmer sur l'appareil. */
#define TONGOU_URL_RELAY    "http://%s/cm?cmnd=POWER"
#define TK_RELAY            "POWER"

#define TONGOU_BUF          1536

static char s_buf[TONGOU_BUF];

esp_err_t drv_tongou_parse_power(const char *json, size_t len, float *power_w)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *sns = cJSON_GetObjectItemCaseSensitive(root, TK_STATUS_SNS);
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(sns, TK_ENERGY);
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(en, TK_POWER);
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsNumber(p)) {
        *power_w = (float)p->valuedouble;
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t drv_tongou_parse_relay(const char *json, size_t len, bool *on)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, TK_RELAY);
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    if (cJSON_IsString(r)) {
        if (strcmp(r->valuestring, "ON") == 0) {
            *on = true;
            err = ESP_OK;
        } else if (strcmp(r->valuestring, "OFF") == 0) {
            *on = false;
            err = ESP_OK;
        }
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t get(const char *fmt, const char *ip, size_t *len)
{
    char url[64];
    snprintf(url, sizeof(url), fmt, ip);
    const http_req_t req = { .url = url, .tls = HTTP_TLS_NONE };
    int status;
    esp_err_t err = http_get(&req, s_buf, sizeof(s_buf), len, &status);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "%s : %s (HTTP %d)", url, esp_err_to_name(err), status);
    }
    return err;
}

#define TONGOU_URL_ON       "http://%s/cm?cmnd=POWER%%20ON"
#define TONGOU_URL_OFF      "http://%s/cm?cmnd=POWER%%20OFF"

/* Lecture (task_poll) et commande (task_act) se partagent les tampons du pilote, et l'appareil n'accepte
 * que peu de connexions simultanées : un seul échange à la fois. */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
#define TONGOU_LOCK_MS      10000

esp_err_t drv_tongou_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    return s_lock != NULL ? ESP_OK : ESP_FAIL;
}

static bool lock(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(TONGOU_LOCK_MS)) == pdTRUE;
}

esp_err_t drv_tongou_set(const cfg_t *cfg, bool on)
{
    if (!lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err;
    if (cfg->tuya.type == CE_DEV_TUYA) {
        err = drv_tuya_set(cfg, on);
    } else {
        size_t len;
        err = get(on ? TONGOU_URL_ON : TONGOU_URL_OFF, cfg->dev.tongou_ip, &len);
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t poll_locked(const cfg_t *cfg, tongou_reading_t *out);

esp_err_t drv_tongou_poll(const cfg_t *cfg, tongou_reading_t *out)
{
    if (!lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = poll_locked(cfg, out);
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t poll_locked(const cfg_t *cfg, tongou_reading_t *out)
{
    if (cfg->tuya.type == CE_DEV_TUYA) {
        tuya_reading_t r;
        esp_err_t terr = drv_tuya_poll(cfg, &r);
        if (terr == ESP_OK) {
            out->relay_on = r.relay_on;
            out->power_w = r.power_w;
        }
        return terr;
    }
    size_t len;
    esp_err_t err = get(TONGOU_URL_ENERGY, cfg->dev.tongou_ip, &len);
    if (err == ESP_OK) {
        err = drv_tongou_parse_power(s_buf, len, &out->power_w);
    }
    if (err == ESP_OK) {
        err = get(TONGOU_URL_RELAY, cfg->dev.tongou_ip, &len);
    }
    if (err == ESP_OK) {
        err = drv_tongou_parse_relay(s_buf, len, &out->relay_on);
    }
    return err;
}
