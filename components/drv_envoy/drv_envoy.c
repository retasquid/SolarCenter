#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "http_util.h"
#include "drv_envoy.h"

static const char *TAG = "drv_envoy";

#define ENVOY_PATH      "/production.json?details=1"
/* Constaté le 2026-11-04 (fw D8.3) : avec jeton, production.json?details=1 répond en plus de 3 s.
 * Lu dans task_poll, séparée du Shelly : ce délai ne retarde pas le capteur maître. */
#define ENVOY_TIMEOUT_MS 8000


static bool str_is(const cJSON *o, const char *k, const char *val)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(it) && strcmp(it->valuestring, val) == 0;
}

esp_err_t drv_envoy_parse(const char *json, size_t len, envoy_reading_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *prod = cJSON_GetObjectItemCaseSensitive(root, "production");
    const cJSON *eim = NULL;
    const cJSON *inv = NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, prod) {
        if (eim == NULL && str_is(e, "type", "eim") && str_is(e, "measurementType", "production")) {
            eim = e;
        } else if (inv == NULL && str_is(e, "type", "inverters")) {
            inv = e;
        }
    }

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    const cJSON *src = eim != NULL ? eim : inv;
    const cJSON *w = src != NULL ? cJSON_GetObjectItemCaseSensitive(src, "wNow") : NULL;
    if (cJSON_IsNumber(w)) {
        out->w_now = (float)w->valuedouble;
        out->from_eim = (src == eim);
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t drv_envoy_poll(const cfg_t *cfg, envoy_reading_t *out)
{
    char url[64];
    bool fw7 = cfg->dev.envoy_fw7;
    snprintf(url, sizeof(url), "%s://%s" ENVOY_PATH, fw7 ? "https" : "http", cfg->dev.envoy_ip);
    const http_req_t req = {
        .url = url,
        .bearer = fw7 ? cfg->dev.envoy_token : NULL,
        /* Vérification maintenue : le certificat auto-signé sera refusé par le bundle. */
        .tls = !fw7 ? HTTP_TLS_NONE : (cfg->dev.envoy_skip_cert ? HTTP_TLS_INSECURE : HTTP_TLS_BUNDLE),
        .timeout_ms = ENVOY_TIMEOUT_MS,
    };
    size_t len;
    int status;
    if (!http_shared_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = http_get(&req, g_http_shared_buf, HTTP_SHARED_BUF_LEN, &len, &status);
    if (err == ESP_OK) {
        err = drv_envoy_parse(g_http_shared_buf, len, out);
    }
    http_shared_unlock();
    if (err == ESP_ERR_INVALID_RESPONSE && status == 401) {
        ESP_LOGW(TAG, "jeton refusé (HTTP 401) : vérifier ou renouveler le jeton dans les Paramètres");
    } else if (err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGW(TAG, "réponse plus grande que %d o", HTTP_SHARED_BUF_LEN);
    } else if (err != ESP_OK) {
        ESP_LOGD(TAG, "lecture : %s (HTTP %d)", esp_err_to_name(err), status);
    }
    return err;
}
