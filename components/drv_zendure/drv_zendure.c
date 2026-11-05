#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "http_util.h"
#include "drv_zendure.h"

static const char *TAG = "drv_zendure";

#define ZENDURE_URL_FMT "http://%s/properties/report"
#define ZENDURE_BUF     4096

/* Seul properties.electricLevel est donné avec son chemin dans le CLAUDE.md.
 * TODO(verify) : les autres champs sont supposés dans le même objet "properties". */
#define ZK_PROPS            "properties"
#define ZK_SOC              "electricLevel"
#define ZK_OUTPUT_HOME      "outputHomePower"   /* TODO(verify) : chemin */
#define ZK_GRID_INPUT       "gridInputPower"    /* TODO(verify) : chemin */
#define ZK_PACK_INPUT       "packInputPower"    /* TODO(verify) : chemin */
#define ZK_OUTPUT_PACK      "outputPackPower"   /* TODO(verify) : chemin */
#define ZK_AC_MODE          "acMode"            /* TODO(verify) : chemin */
#define ZK_INPUT_LIMIT      "inputLimit"        /* TODO(verify) : chemin */
#define ZK_OUTPUT_LIMIT     "outputLimit"       /* TODO(verify) : chemin */
#define ZK_SOC_SET          "socSet"            /* TODO(verify) : chemin */
#define ZK_MIN_SOC          "minSoc"            /* TODO(verify) : chemin */

static char s_buf[ZENDURE_BUF];

static bool get_num(const cJSON *o, const char *k, float *dst)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(it)) {
        return false;
    }
    *dst = (float)it->valuedouble;
    return true;
}

static bool get_int(const cJSON *o, const char *k, int *dst)
{
    float v;
    if (!get_num(o, k, &v)) {
        return false;
    }
    *dst = (int)v;
    return true;
}

esp_err_t drv_zendure_parse(const char *json, size_t len, zendure_reading_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memset(out, 0, sizeof(*out));
    const cJSON *sn = cJSON_GetObjectItemCaseSensitive(root, "sn");
    if (cJSON_IsString(sn)) {
        strlcpy(out->sn, sn->valuestring, sizeof(out->sn));
    }
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, ZK_PROPS);
    bool ok = cJSON_IsObject(p) &&
              get_num(p, ZK_SOC, &out->soc) &&
              get_num(p, ZK_OUTPUT_HOME, &out->output_home_w) &&
              get_num(p, ZK_GRID_INPUT, &out->grid_input_w);
    if (ok) {
        /* Champs informatifs : absents = 0. */
        get_num(p, ZK_PACK_INPUT, &out->pack_input_w);
        get_num(p, ZK_OUTPUT_PACK, &out->output_pack_w);
        get_int(p, ZK_AC_MODE, &out->ac_mode);
        get_num(p, ZK_INPUT_LIMIT, &out->input_limit_w);
        get_num(p, ZK_OUTPUT_LIMIT, &out->output_limit_w);
        get_int(p, ZK_SOC_SET, &out->soc_set_pm);
        get_int(p, ZK_MIN_SOC, &out->min_soc_pm);
        /* TODO(verify) : p_batt = outputHomePower − gridInputPower, à comparer au Shelly
         * lors d'une charge et d'une décharge forcées (§7). */
        out->p_batt = out->output_home_w - out->grid_input_w;
    }
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* ---------- Écriture (§8.3) ---------- */

#define ZENDURE_WRITE_FMT   "http://%s/properties/write"
/* TODO(verify) : sémantique de smartMode (le CLAUDE.md l'indique pour limiter l'usure de la flash de la
 * batterie lors d'écritures fréquentes ; le champ existe dans properties/report, valeur 1 constatée). */
#define ZK_SMART_MODE       "smartMode"
#define ZENDURE_SMART_MODE  1
#define ZENDURE_AC_CHARGE   1
#define ZENDURE_AC_DISCHARGE 2

static esp_err_t write_props(const cfg_t *cfg, const char *props)
{
    if (cfg->dev.zendure_sn[0] == '\0') {
        ESP_LOGW(TAG, "numéro de série non renseigné : écriture impossible");
        return ESP_ERR_INVALID_STATE;
    }
    char url[64];
    static char body[192];
    snprintf(url, sizeof(url), ZENDURE_WRITE_FMT, cfg->dev.zendure_ip);
    int n = snprintf(body, sizeof(body), "{\"sn\":\"%s\",\"properties\":{%s}}", cfg->dev.zendure_sn, props);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const http_req_t req = { .url = url, .tls = HTTP_TLS_NONE };
    size_t len;
    int status;
    static char resp[512];      /* distinct de s_buf : l'écriture (task_act) et la lecture (task_poll) sont concurrentes */
    esp_err_t err = http_post_json(&req, body, resp, sizeof(resp), &len, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "écriture %s : %s (HTTP %d)", props, esp_err_to_name(err), status);
    }
    return err;
}

esp_err_t drv_zendure_set_power(const cfg_t *cfg, float watts)
{
    char props[96];
    int w = (int)lroundf(fabsf(watts));
    if (w == 0) {
        snprintf(props, sizeof(props), "\"inputLimit\":0,\"outputLimit\":0,\"%s\":%d", ZK_SMART_MODE,
                 ZENDURE_SMART_MODE);
    } else if (watts > 0.0f) {
        snprintf(props, sizeof(props), "\"acMode\":%d,\"outputLimit\":%d,\"%s\":%d", ZENDURE_AC_DISCHARGE, w,
                 ZK_SMART_MODE, ZENDURE_SMART_MODE);
    } else {
        snprintf(props, sizeof(props), "\"acMode\":%d,\"inputLimit\":%d,\"%s\":%d", ZENDURE_AC_CHARGE, w,
                 ZK_SMART_MODE, ZENDURE_SMART_MODE);
    }
    return write_props(cfg, props);
}

esp_err_t drv_zendure_set_soc_limits(const cfg_t *cfg)
{
    char props[64];
    /* ‰ : 1000 = 100 % (§8.3). */
    snprintf(props, sizeof(props), "\"minSoc\":%d,\"socSet\":%d", cfg->batt.soc_min * 10, cfg->batt.soc_max * 10);
    return write_props(cfg, props);
}

esp_err_t drv_zendure_poll(const cfg_t *cfg, zendure_reading_t *out)
{
    char url[64];
    snprintf(url, sizeof(url), ZENDURE_URL_FMT, cfg->dev.zendure_ip);
    const http_req_t req = { .url = url, .tls = HTTP_TLS_NONE };
    size_t len;
    int status;
    esp_err_t err = http_get(&req, s_buf, sizeof(s_buf), &len, &status);
    if (err == ESP_OK) {
        err = drv_zendure_parse(s_buf, len, out);
    }
    if (err == ESP_OK && cfg->dev.zendure_sn[0] != '\0' && strcmp(out->sn, cfg->dev.zendure_sn) != 0) {
        ESP_LOGW(TAG, "numéro de série lu « %s » différent de la config", out->sn);
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGW(TAG, "réponse plus grande que %d o", ZENDURE_BUF);
    } else if (err != ESP_OK) {
        ESP_LOGD(TAG, "lecture : %s (HTTP %d)", esp_err_to_name(err), status);
    }
    return err;
}
