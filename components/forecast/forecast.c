#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "cJSON.h"
#include "app_state.h"
#include "config.h"
#include "http_util.h"
#include "models.h"
#include "forecast.h"

static const char *TAG = "forecast";

/* Paramètres vérifiés le 2026-11-04 : documentation officielle (convention d'azimut :
 * 0 = sud, −90 = est, +90 = ouest) et essais (tilt et azimuth modifient bien la réponse). */
#define OM_URL_FMT  "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f" \
                    "&hourly=global_tilted_irradiance,temperature_2m&tilt=%.0f&azimuth=%.0f" \
                    "&timezone=auto&forecast_days=2&timeformat=unixtime"
#define OM_BUF              4096        /* réponse constatée : ~1,3 Ko */
#define OM_TIMEOUT_MS       10000
#define GTI_CENTER_OFFSET_S (-1800)     /* moyenne de l'heure précédente */

#define REFRESH_S           3600
#define RETRY_S             300
#define MORNING_MIN         (5 * 60 + 30)   /* 05:30 */
#define TICK_MS             10000       /* < délai du Task Watchdog (30 s) */

#define FC_STACK            6144
#define FC_PRIO             3
#define FC_CORE             0

static forecast_t s_fc;
static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx;
static char s_buf[OM_BUF];
static cfg_t s_cfg;
static TaskHandle_t s_task;
static StaticTask_t s_task_buf;
static StackType_t s_task_stack[FC_STACK];

static bool read_series(const cJSON *hourly, const char *key, float *dst, int n)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(hourly, key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < n) {
        return false;
    }
    int i = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, arr) {
        if (i >= n) {
            break;
        }
        dst[i++] = cJSON_IsNumber(v) ? (float)v->valuedouble : NAN;
    }
    return true;
}

esp_err_t forecast_parse(const char *json, size_t len, forecast_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *time_arr = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    const cJSON *first = cJSON_GetArrayItem(time_arr, 0);
    int n = cJSON_GetArraySize(time_arr);
    if (n > FORECAST_HOURS) {
        n = FORECAST_HOURS;
    }
    if (cJSON_IsNumber(first) && n > 0 &&
        read_series(hourly, "global_tilted_irradiance", out->gti, n) &&
        read_series(hourly, "temperature_2m", out->temp, n)) {
        out->t0 = (int64_t)first->valuedouble;
        out->n = (uint8_t)n;
        out->valid = true;
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t fetch(void)
{
    char url[320];
    snprintf(url, sizeof(url), OM_URL_FMT, s_cfg.pv.lat, s_cfg.pv.lon, s_cfg.pv.tilt, s_cfg.pv.azimuth);
    const http_req_t req = { .url = url, .tls = HTTP_TLS_BUNDLE, .timeout_ms = OM_TIMEOUT_MS };
    size_t len;
    int status;
    esp_err_t err = http_get(&req, s_buf, sizeof(s_buf), &len, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Open-Meteo : %s (HTTP %d)", esp_err_to_name(err), status);
        return err;
    }
    static forecast_t fc;      /* hors pile */
    memset(&fc, 0, sizeof(fc));
    err = forecast_parse(s_buf, len, &fc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "réponse Open-Meteo illisible");
        return err;
    }
    fc.fetched = time(NULL);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_fc = fc;
    xSemaphoreGive(s_mtx);

    float sum = 0.0f;
    for (int i = 0; i < fc.n; i++) {
        sum += isfinite(fc.gti[i]) ? fc.gti[i] : 0.0f;
    }
    ESP_LOGI(TAG, "prévision reçue : %d h, irradiance cumulée %.0f Wh/m²", fc.n, sum);
    return ESP_OK;
}

static bool coords_set(void)
{
    return !(s_cfg.pv.lat == 0.0f && s_cfg.pv.lon == 0.0f);
}

static void forecast_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    int64_t next_try = 0;
    int last_morning_yday = -1;
    bool warned = false;

    for (;;) {
        bool forced = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS)) > 0;
        esp_task_wdt_reset();

        app_state_t st;
        app_state_get(&st);
        if (!st.time_synced || !st.wifi_connected) {
            continue;
        }
        config_get(&s_cfg);
        if (!coords_set()) {
            if (!warned) {
                ESP_LOGW(TAG, "latitude/longitude non renseignées : pas de prévision");
                warned = true;
            }
            continue;
        }
        warned = false;

        int64_t now = time(NULL);
        struct tm tm;
        time_t tt = (time_t)now;
        localtime_r(&tt, &tm);
        bool morning = tm.tm_hour * 60 + tm.tm_min >= MORNING_MIN && tm.tm_yday != last_morning_yday;
        if (forced || morning || now >= next_try) {
            esp_err_t err = fetch();
            esp_task_wdt_reset();
            next_try = now + (err == ESP_OK ? REFRESH_S : RETRY_S);
            if (err == ESP_OK && morning) {
                last_morning_yday = tm.tm_yday;
            }
        }
    }
}

esp_err_t forecast_start(void)
{
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    if (s_mtx == NULL) {
        return ESP_FAIL;
    }
    s_task = xTaskCreateStaticPinnedToCore(forecast_task, "task_forecast", FC_STACK, NULL, FC_PRIO,
                                           s_task_stack, &s_task_buf, FC_CORE);
    return s_task != NULL ? ESP_OK : ESP_FAIL;
}

void forecast_get(forecast_t *out)
{
    if (s_mtx == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(out, &s_fc, sizeof(*out));
    xSemaphoreGive(s_mtx);
}

bool forecast_at(int64_t t, float *gti, float *temp)
{
    if (s_mtx == NULL) {
        return false;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool ok = s_fc.valid;
    if (ok) {
        *gti = models_hourly_at(s_fc.gti, s_fc.n, s_fc.t0, GTI_CENTER_OFFSET_S, t);
        *temp = models_hourly_at(s_fc.temp, s_fc.n, s_fc.t0, 0, t);
        ok = isfinite(*gti);
    }
    xSemaphoreGive(s_mtx);
    return ok;
}

void forecast_refresh(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}
