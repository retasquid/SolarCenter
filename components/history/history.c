#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "config.h"
#include "forecast.h"
#include "tariff.h"
#include "profile.h"
#include "models.h"
#include "history.h"

static const char *TAG = "history";

#define HIST_DIR            "/fs/hist"
#define SLOTS_FILE          HIST_DIR "/slots1.bin"
#define DAYS_FILE           HIST_DIR "/days1.bin"
#define MONTHS_FILE         HIST_DIR "/months1.bin"
#define MONTH_BASE_YEAR     2024
#define LEARN_MIN_COVER     0.8f        /* part du créneau mesurée pour apprendre */
#define SOC_UNKNOWN         255

#define NVS_NS              "models"
#define MODELS_VERSION      1
#define MODELS_SAVE_SLOTS   4           /* sauvegarde NVS toutes les 2 h */

#define HIST_STACK          6144
#define HIST_PRIO           2
#define HIST_CORE           0
#define HIST_TICK_MS        10000       /* < délai du Task Watchdog (30 s) */

/* ---------- État ---------- */

/* Accumulation (s_mtx) */
static energy_acc_t s_slot;             /* créneau en cours */
static uint32_t s_slot_start;
static energy_acc_t s_today;            /* créneaux clos d'aujourd'hui */
static uint32_t s_today_date;           /* AAAAMMJJ, 0 = pas encore restauré */
static energy_acc_t s_pending;          /* créneau clos en attente de task_hist */
static uint32_t s_pending_start;
static uint8_t s_pending_soc;
static bool s_pending_valid;
static flows_in_t s_prev;
static uint32_t s_prev_ms;
static bool s_prev_valid;
static bool s_prev_full;
static uint8_t s_soc = SOC_UNKNOWN;
static tariff_t s_tariff;
static float s_kwc = 1.0f;
static bool s_pv_configured;

/* Modèles (s_model_mtx) */
static rls_t s_rls_pv;
static rls_t s_rls_base;
static profile_t s_profile;

static StaticSemaphore_t s_mtx_buf, s_model_mtx_buf, s_file_mtx_buf;
static SemaphoreHandle_t s_mtx, s_model_mtx, s_file_mtx;
static TaskHandle_t s_task;
static StaticTask_t s_task_buf;
static StackType_t s_task_stack[HIST_STACK];
static bool s_started;
static cfg_t s_cfg;

/* ---------- Dates locales ---------- */

/* Algorithmes de H. Hinnant (calendrier grégorien proleptique). */
static int32_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

int32_t history_daynum(uint32_t date)
{
    return days_from_civil((int)(date / 10000), (date / 100) % 100, date % 100);
}

uint32_t history_date_from_daynum(int32_t z)
{
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const int y = (int)yoe + era * 400 + (m <= 2);
    return (uint32_t)y * 10000 + m * 100 + d;
}

static void local_tm(int64_t t, struct tm *tm)
{
    time_t tt = (time_t)t;
    localtime_r(&tt, tm);
}

uint32_t history_date_of(int64_t t)
{
    struct tm tm;
    local_tm(t, &tm);
    return (uint32_t)(tm.tm_year + 1900) * 10000 + (uint32_t)(tm.tm_mon + 1) * 100 + (uint32_t)tm.tm_mday;
}

static uint32_t slot_start_of(int64_t t)
{
    struct tm tm;
    local_tm(t, &tm);
    return (uint32_t)(t - (tm.tm_min % 30) * 60 - tm.tm_sec);
}

/* ---------- Fichiers ---------- */

static esp_err_t file_write_at(const char *path, long off, const void *data, size_t len)
{
    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        f = fopen(path, "w+b");
    }
    if (f == NULL) {
        return ESP_FAIL;
    }
    bool ok = fseek(f, off, SEEK_SET) == 0 && fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return ok ? ESP_OK : ESP_FAIL;
}

static bool file_read_at(FILE *f, long off, void *data, size_t len)
{
    return fseek(f, off, SEEK_SET) == 0 && fread(data, 1, len, f) == len;
}

static bool read_one(const char *path, long off, void *data, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = file_read_at(f, off, data, len);
    fclose(f);
    return ok;
}

static long slot_off(uint32_t ts)
{
    return (long)((ts / HIST_SLOT_S) % HIST_SLOTS_KEPT) * (long)sizeof(hist_slot_t);
}

static long day_off(uint32_t date)
{
    int32_t dn = history_daynum(date);
    return (long)(((dn % HIST_DAYS_KEPT) + HIST_DAYS_KEPT) % HIST_DAYS_KEPT) * (long)sizeof(hist_day_t);
}

static long month_off(uint32_t yyyymm)
{
    int idx = ((int)(yyyymm / 100) - MONTH_BASE_YEAR) * 12 + (int)(yyyymm % 100) - 1;
    return idx < 0 ? -1 : (long)idx * (long)sizeof(hist_month_t);
}

static void write_day(uint32_t date, const energy_acc_t *acc)
{
    hist_day_t d = { .date = date, .acc = *acc };
    xSemaphoreTake(s_file_mtx, portMAX_DELAY);
    esp_err_t err = file_write_at(DAYS_FILE, day_off(date), &d, sizeof(d));
    xSemaphoreGive(s_file_mtx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "écriture du jour %lu impossible", (unsigned long)date);
    }
}

static void add_to_month(uint32_t date, const energy_acc_t *acc)
{
    uint32_t ym = date / 100;
    long off = month_off(ym);
    if (off < 0) {
        return;
    }
    hist_month_t m;
    xSemaphoreTake(s_file_mtx, portMAX_DELAY);
    if (!read_one(MONTHS_FILE, off, &m, sizeof(m)) || m.month != ym) {
        memset(&m, 0, sizeof(m));
        m.month = ym;
    }
    stats_add(&m.acc, acc);
    m.days++;
    esp_err_t err = file_write_at(MONTHS_FILE, off, &m, sizeof(m));
    xSemaphoreGive(s_file_mtx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "écriture du mois %lu impossible", (unsigned long)ym);
    }
}

/* ---------- Modèles (NVS) ---------- */

static void models_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS modèles : %s", esp_err_to_name(err));
        return;
    }
    xSemaphoreTake(s_model_mtx, portMAX_DELAY);
    err = nvs_set_u8(h, "ver", MODELS_VERSION);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "pv", &s_rls_pv, sizeof(s_rls_pv));
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "base", &s_rls_base, sizeof(s_rls_base));
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "prof", &s_profile, sizeof(s_profile));
    }
    xSemaphoreGive(s_model_mtx);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sauvegarde des modèles : %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "modèles sauvegardés (PV %lu éch., base %lu éch.)",
                 (unsigned long)s_rls_pv.samples, (unsigned long)s_rls_base.samples);
    }
}

static bool load_blob(nvs_handle_t h, const char *key, void *dst, size_t len)
{
    size_t got = len;
    return nvs_get_blob(h, key, dst, &got) == ESP_OK && got == len;
}

static void models_load(float kwc)
{
    models_pv_init(&s_rls_pv, kwc);
    models_base_init(&s_rls_base);
    profile_init(&s_profile);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "aucun modèle enregistré : valeurs initiales");
        return;
    }
    uint8_t ver = 0;
    if (nvs_get_u8(h, "ver", &ver) == ESP_OK && ver == MODELS_VERSION) {
        static rls_t tmp;
        if (load_blob(h, "pv", &tmp, sizeof(tmp))) {
            s_rls_pv = tmp;
        }
        if (load_blob(h, "base", &tmp, sizeof(tmp))) {
            s_rls_base = tmp;
        }
        static profile_t ptmp;
        if (load_blob(h, "prof", &ptmp, sizeof(ptmp))) {
            s_profile = ptmp;
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "modèles chargés : PV %lu éch., base %lu éch.",
             (unsigned long)s_rls_pv.samples, (unsigned long)s_rls_base.samples);
}

/* ---------- Accumulation ---------- */

static void rollover_locked(int64_t now)
{
    uint32_t start = slot_start_of(now);
    if (s_slot_start == 0 || start < s_slot_start) {
        /* Premier créneau, ou horloge reculée par une resynchronisation. */
        memset(&s_slot, 0, sizeof(s_slot));
        s_slot_start = start;
        return;
    }
    if (start == s_slot_start) {
        return;
    }
    if (s_pending_valid) {
        ESP_LOGW(TAG, "créneau précédent non traité, remplacé");
    }
    s_pending = s_slot;
    s_pending_start = s_slot_start;
    s_pending_soc = s_soc;
    s_pending_valid = true;
    memset(&s_slot, 0, sizeof(s_slot));
    s_slot_start = start;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void history_on_sample(const app_state_t *st)
{
    if (!s_started || !st->time_synced) {
        return;
    }
    flows_in_t in;
    bool full;
    bool grid_ok = app_state_flows_in(st, &in, &full);
    uint32_t ms = st->p_grid.ts_ms;
    int64_t now = time(NULL);
    struct tm tm;
    local_tm(now, &tm);
    uint16_t minute = (uint16_t)(tm.tm_hour * 60 + tm.tm_min);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    rollover_locked(now);
    if (grid_ok && ms != s_prev_ms) {
        if (s_prev_valid) {
            float dt = (float)(ms - s_prev_ms) / 1000.0f;
            stats_integrate(&s_slot, &s_prev, &in, dt, full && s_prev_full, tariff_price(&s_tariff, minute),
                            s_tariff.price_sell, s_tariff.price_hc);
        }
        s_prev = in;
        s_prev_ms = ms;
        s_prev_full = full;
        s_prev_valid = true;
    } else if (!grid_ok) {
        s_prev_valid = false;
    }
    if (app_state_usable(st, DEV_ZENDURE, &st->soc)) {
        s_soc = (uint8_t)lroundf(st->soc.value);
    }
    xSemaphoreGive(s_mtx);
}

bool history_today(energy_acc_t *out, uint32_t *date)
{
    memset(out, 0, sizeof(*out));
    if (!s_started) {
        return false;
    }
    int64_t now = time(NULL);
    if (now < 1700000000) {
        return false;       /* heure pas encore synchronisée */
    }
    uint32_t d = history_date_of(now);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_today_date == d) {
        *out = s_today;
    }
    if (s_pending_valid && history_date_of(s_pending_start) == d) {
        stats_add(out, &s_pending);
    }
    if (s_slot_start != 0 && history_date_of(s_slot_start) == d) {
        stats_add(out, &s_slot);
    }
    xSemaphoreGive(s_mtx);
    if (date != NULL) {
        *date = d;
    }
    return true;
}

/* ---------- Prévisions et apprentissage ---------- */

static void predict_locked(uint32_t slot_ts, hist_pred_t *p)
{
    float gti = NAN, temp = NAN;
    bool fc = forecast_at((int64_t)slot_ts + HIST_SLOT_S / 2, &gti, &temp);
    struct tm tm;
    local_tm(slot_ts, &tm);
    int dtype = profile_day_type(tm.tm_wday);
    int idx = (tm.tm_hour * 60 + tm.tm_min) / 30;

    p->gti = fc ? gti : NAN;
    p->temp = fc ? temp : NAN;
    p->pv_raw = fc ? models_pv_fallback(s_kwc, gti > 0.0f ? gti : 0.0f) : NAN;
    p->pv = fc ? models_pv_predict(&s_rls_pv, s_kwc, gti, temp) : NAN;
    p->base_prof = profile_get(&s_profile, dtype, idx);
    p->base = models_base_predict(&s_rls_base, p->base_prof, p->temp);
}

bool history_predict(uint32_t slot_ts, hist_pred_t *out)
{
    if (!s_started) {
        return false;
    }
    xSemaphoreTake(s_model_mtx, portMAX_DELAY);
    predict_locked(slot_ts, out);
    xSemaphoreGive(s_model_mtx);
    return isfinite(out->pv);
}

static void learn_locked(uint32_t start, const energy_acc_t *acc, const hist_pred_t *p)
{
    if (acc->secs_full < LEARN_MIN_COVER * HIST_SLOT_S) {
        return;
    }
    struct tm tm;
    local_tm(start, &tm);
    int dtype = profile_day_type(tm.tm_wday);
    int idx = (tm.tm_hour * 60 + tm.tm_min) / 30;

    /* Consommation de base (hors chauffe-eau). */
    float base_wh = acc->e_home - acc->e_ce;
    if (base_wh >= 0.0f && base_wh <= MODELS_BASE_MAX_WH) {
        if (isfinite(p->temp)) {
            float x[3];
            models_base_x(p->base_prof, p->temp, x);
            if (rls_update(&s_rls_base, x, base_wh) == RLS_RESET) {
                ESP_LOGW(TAG, "modèle de consommation divergent : réinitialisé");
            }
        }
        profile_update(&s_profile, dtype, idx, base_wh);
    }

    /* Production PV : seulement Envoy présent et irradiance prévue suffisante (§9.2 a). */
    if (s_pv_configured && isfinite(p->gti) && p->gti > MODELS_PV_GTI_MIN &&
        acc->e_pv >= 0.0f && acc->e_pv <= models_pv_max(s_kwc)) {
        float x[3];
        models_pv_x(p->gti, p->temp, x);
        if (rls_update(&s_rls_pv, x, acc->e_pv) == RLS_RESET) {
            ESP_LOGW(TAG, "modèle PV divergent : réinitialisé");
        }
    }
}

static uint8_t cover_pct(float secs)
{
    float pct = secs * 100.0f / HIST_SLOT_S;
    return (uint8_t)(pct > 100.0f ? 100.0f : (pct < 0.0f ? 0.0f : pct));
}

static void finalize_slot(uint32_t start, const energy_acc_t *acc, uint8_t soc)
{
    hist_pred_t p;
    xSemaphoreTake(s_model_mtx, portMAX_DELAY);
    predict_locked(start, &p);
    learn_locked(start, acc, &p);
    xSemaphoreGive(s_model_mtx);

    hist_slot_t rec = {
        .ts = start,
        .e_pv = acc->e_pv,
        .e_import = acc->e_import,
        .e_export = acc->e_export,
        .e_home = acc->e_home,
        .e_batt_ch = acc->e_batt_ch,
        .e_batt_dis = acc->e_batt_dis,
        .e_ce = acc->e_ce,
        .pv_pred = p.pv,
        .base_pred = p.base,
        .soc = soc,
        .cov_grid = cover_pct(acc->secs_grid),
        .cov_full = cover_pct(acc->secs_full),
    };
    xSemaphoreTake(s_file_mtx, portMAX_DELAY);
    esp_err_t err = file_write_at(SLOTS_FILE, slot_off(start), &rec, sizeof(rec));
    xSemaphoreGive(s_file_mtx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "écriture du créneau impossible");
    }

    /* Journée : la précédente est close au premier créneau du jour suivant. */
    uint32_t date = history_date_of(start);
    bool close = false;
    static energy_acc_t closed;
    uint32_t closed_date = 0;
    static energy_acc_t today;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_today_date != date) {
        if (s_today_date != 0) {
            closed = s_today;
            closed_date = s_today_date;
            close = true;
        }
        memset(&s_today, 0, sizeof(s_today));
        s_today_date = date;
    }
    stats_add(&s_today, acc);
    today = s_today;
    xSemaphoreGive(s_mtx);

    /* Sauvegarde des modèles : à chaque journée close et toutes les 2 h, pour qu'un redémarrage ne
     * perde pas l'apprentissage du jour (le §9.2 ne prévoit qu'une sauvegarde quotidienne). */
    static int slots_since_save;
    if (close || ++slots_since_save >= MODELS_SAVE_SLOTS) {
        slots_since_save = 0;
        models_save();
    }
    if (close) {
        write_day(closed_date, &closed);
        add_to_month(closed_date, &closed);
        ESP_LOGI(TAG, "journée %lu close : PV %.2f kWh, import %.2f kWh, export %.2f kWh",
                 (unsigned long)closed_date, closed.e_pv / 1000.0f, closed.e_import / 1000.0f,
                 closed.e_export / 1000.0f);
    }
    write_day(date, &today);
    ESP_LOGI(TAG, "créneau clos : PV %.0f Wh, maison %.0f Wh, couverture %u %%",
             acc->e_pv, acc->e_home, rec.cov_full);
}

/* Reprend les énergies du jour enregistrées avant un redémarrage. */
static void restore_today(int64_t now)
{
    uint32_t date = history_date_of(now);
    hist_day_t d;
    xSemaphoreTake(s_file_mtx, portMAX_DELAY);
    bool ok = read_one(DAYS_FILE, day_off(date), &d, sizeof(d)) && d.date == date;
    xSemaphoreGive(s_file_mtx);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_today_date == 0) {
        if (ok) {
            s_today = d.acc;
        }
        s_today_date = date;
    }
    xSemaphoreGive(s_mtx);
    if (ok) {
        ESP_LOGI(TAG, "énergies du jour reprises (PV %.2f kWh)", d.acc.e_pv / 1000.0f);
    }
}

static void refresh_cfg(void)
{
    config_get(&s_cfg);
    tariff_t t;
    config_tariff(&s_cfg, &t);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_tariff = t;
    s_kwc = s_cfg.pv.kwc;
    s_pv_configured = s_cfg.dev.envoy_ip[0] != '\0';
    xSemaphoreGive(s_mtx);
}

static void hist_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    bool restored = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HIST_TICK_MS));
        esp_task_wdt_reset();
        refresh_cfg();

        int64_t now = time(NULL);
        if (now < 1700000000) {
            continue;       /* pas encore d'heure NTP */
        }
        if (!restored) {
            restore_today(now);
            restored = true;
        }

        static energy_acc_t acc;
        uint32_t start = 0;
        uint8_t soc = SOC_UNKNOWN;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        rollover_locked(now);       /* clôt aussi les créneaux sans mesure */
        bool pending = s_pending_valid;
        if (pending) {
            acc = s_pending;
            start = s_pending_start;
            soc = s_pending_soc;
            s_pending_valid = false;
        }
        xSemaphoreGive(s_mtx);

        if (pending) {
            finalize_slot(start, &acc, soc);
            esp_task_wdt_reset();
        }
    }
}

esp_err_t history_start(void)
{
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    s_model_mtx = xSemaphoreCreateMutexStatic(&s_model_mtx_buf);
    s_file_mtx = xSemaphoreCreateMutexStatic(&s_file_mtx_buf);
    if (s_mtx == NULL || s_model_mtx == NULL || s_file_mtx == NULL) {
        return ESP_FAIL;
    }
    struct stat sb;
    if (stat(HIST_DIR, &sb) != 0 && mkdir(HIST_DIR, 0775) != 0) {
        ESP_LOGE(TAG, "création de %s impossible : historique désactivé", HIST_DIR);
    }
    refresh_cfg();
    models_load(s_kwc);
    s_task = xTaskCreateStaticPinnedToCore(hist_task, "task_hist", HIST_STACK, NULL, HIST_PRIO,
                                           s_task_stack, &s_task_buf, HIST_CORE);
    if (s_task == NULL) {
        return ESP_FAIL;
    }
    s_started = true;
    return ESP_OK;
}

/* ---------- Lecture ---------- */

esp_err_t history_foreach_slot(uint32_t from_ts, uint32_t to_ts, hist_slot_cb_t cb, void *ctx)
{
    xSemaphoreTake(s_file_mtx, portMAX_DELAY);
    FILE *f = fopen(SLOTS_FILE, "rb");
    if (f == NULL) {
        xSemaphoreGive(s_file_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    hist_slot_t rec;
    uint32_t ts = from_ts - from_ts % HIST_SLOT_S;
    for (; ts <= to_ts; ts += HIST_SLOT_S) {
        if (file_read_at(f, slot_off(ts), &rec, sizeof(rec)) && rec.ts == ts && !cb(&rec, ctx)) {
            break;
        }
    }
    fclose(f);
    xSemaphoreGive(s_file_mtx);
    return ESP_OK;
}

esp_err_t history_foreach_day(int n_days, hist_day_cb_t cb, void *ctx)
{
    int32_t today = history_daynum(history_date_of(time(NULL)));
    xSemaphoreTake(s_file_mtx, portMAX_DELAY);
    FILE *f = fopen(DAYS_FILE, "rb");
    if (f == NULL) {
        xSemaphoreGive(s_file_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    hist_day_t rec;
    for (int k = n_days; k >= 1; k--) {
        uint32_t date = history_date_from_daynum(today - k);
        if (file_read_at(f, day_off(date), &rec, sizeof(rec)) && rec.date == date && !cb(&rec, ctx)) {
            break;
        }
    }
    fclose(f);
    xSemaphoreGive(s_file_mtx);
    return ESP_OK;
}

esp_err_t history_foreach_month(hist_month_cb_t cb, void *ctx)
{
    xSemaphoreTake(s_file_mtx, portMAX_DELAY);
    FILE *f = fopen(MONTHS_FILE, "rb");
    if (f == NULL) {
        xSemaphoreGive(s_file_mtx);
        return ESP_ERR_NOT_FOUND;
    }
    hist_month_t rec;
    while (fread(&rec, 1, sizeof(rec), f) == sizeof(rec)) {
        if (rec.month != 0 && !cb(&rec, ctx)) {
            break;
        }
    }
    fclose(f);
    xSemaphoreGive(s_file_mtx);
    return ESP_OK;
}

void history_model_info(rls_t *pv, rls_t *base, int *profile_slots)
{
    xSemaphoreTake(s_model_mtx, portMAX_DELAY);
    *pv = s_rls_pv;
    *base = s_rls_base;
    *profile_slots = profile_learned_slots(&s_profile);
    xSemaphoreGive(s_model_mtx);
}

esp_err_t history_reset_models(bool pv, bool base, bool profile)
{
    xSemaphoreTake(s_model_mtx, portMAX_DELAY);
    if (pv) {
        models_pv_init(&s_rls_pv, s_kwc);
    }
    if (base) {
        models_base_init(&s_rls_base);
    }
    if (profile) {
        profile_init(&s_profile);
    }
    xSemaphoreGive(s_model_mtx);
    ESP_LOGW(TAG, "modèles réinitialisés :%s%s%s", pv ? " PV" : "", base ? " base" : "", profile ? " profil" : "");
    models_save();
    return ESP_OK;
}
