#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "app_state.h"
#include "config.h"
#include "history.h"
#include "stats.h"
#include "tariff.h"
#include "control.h"
#include "control_priv.h"

static const char *TAG = "control";

#define REGUL_PERIOD_MS     5000        /* au plus tard : la tâche est réveillée à chaque mesure (≈ 2 s) */
#define REGUL_MIN_STEP_MS   900         /* Shelly et SolarFlow lus presque ensemble : un seul pas */
#define LOG_SP_GAP_S        10          /* variations de consigne seules : une ligne par 10 s au plus */
#define REGUL_STACK         6144
#define REGUL_PRIO          7
#define REGUL_CORE          1
#define LOG_HEARTBEAT_S     300         /* une ligne au moins toutes les 5 min */
#define LOG_SP_STEP_W       50          /* variation de consigne qui mérite une ligne */
#define PLAN_MORNING_MIN    (5 * 60 + 30)
#define MAX_HC_WINDOWS      12
#define SOC_UNKNOWN         255
#define NVS_NS              "control"
#define NVS_KEY_WH          "wh"

static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx;         /* s_status, s_log, forçage */
static control_status_t s_status;
static ctl_log_t s_log[CONTROL_LOG_LEN];
static int s_log_head;
static int s_log_count;
static reg_override_t s_ovr;
static int64_t s_ovr_until;

/* Propres à task_regul. */
static StaticTask_t s_task_buf;
static StackType_t s_task_stack[REGUL_STACK];
static cfg_t s_cfg;
static reg_cfg_t s_rcfg;
static reg_state_t s_reg;
static plan_in_t s_pin;
static plan_t s_plan;
static bool s_plan_valid;
static arb_t s_arb;                     /* arbitrage réseau de la prochaine plage HC (§9.6) */
static wh_night_t s_night;

/* ---------- Heure locale ---------- */

static int64_t local_at(int64_t now, int day_offset, uint16_t minute)
{
    struct tm tm;
    time_t t = (time_t)now;
    localtime_r(&t, &tm);
    tm.tm_mday += day_offset;
    tm.tm_hour = minute / 60;
    tm.tm_min = minute % 60;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;       /* changement d'heure géré par mktime */
    return (int64_t)mktime(&tm);
}

static uint16_t local_minute(int64_t t)
{
    struct tm tm;
    time_t tt = (time_t)t;
    localtime_r(&tt, &tm);
    return (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
}

static void showers(int64_t now, uint16_t shower_min, int64_t *last, int64_t *next)
{
    int64_t today = local_at(now, 0, shower_min);
    if (now >= today) {
        *last = today;
        *next = local_at(now, 1, shower_min);
    } else {
        *last = local_at(now, -1, shower_min);
        *next = today;
    }
}

/* Plages HC absolues qui recoupent [now, until[. */
static int hc_windows(const tariff_t *t, int64_t now, int64_t until, wh_window_t *w, int max)
{
    int n = 0;
    if (!t->hphc) {
        return 0;
    }
    for (int day = -1; day <= 1; day++) {
        for (int i = 0; i < t->n_hc && i < TARIFF_SLOTS_MAX; i++) {
            uint16_t a = t->hc[i].start_min, b = t->hc[i].end_min;
            if (a == b) {
                continue;
            }
            int64_t s = local_at(now, day, a);
            int64_t e = a < b ? local_at(now, day, b) : local_at(now, day + 1, b);
            if (e > now && s < until && n < max) {
                w[n].start = s;
                w[n].end = e;
                n++;
            }
        }
    }
    return n;
}

/* ---------- Persistance de E_full ---------- */

typedef struct {
    float   e_full_wh;
    int64_t last_full;
} wh_saved_t;

static void wh_load(void)
{
    nvs_handle_t h;
    wh_saved_t v;
    size_t len = sizeof(v);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, NVS_KEY_WH, &v, &len) == ESP_OK && len == sizeof(v) && v.e_full_wh > 0.0f) {
            s_reg.wh.e_full_wh = v.e_full_wh;
            s_reg.wh.last_full = v.last_full;
            ESP_LOGI(TAG, "E_full appris : %.0f Wh", v.e_full_wh);
        }
        nvs_close(h);
    }
}

static void wh_save(void)
{
    nvs_handle_t h;
    wh_saved_t v = { s_reg.wh.e_full_wh, s_reg.wh.last_full };
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(h, NVS_KEY_WH, &v, sizeof(v)) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "sauvegarde de E_full impossible");
    }
    nvs_close(h);
}

/* ---------- Plan ---------- */

static void compute_plan(int64_t now, const app_state_t *st, const tariff_t *tf, int64_t next_shower)
{
    memset(&s_pin, 0, sizeof(s_pin));
    int64_t t0 = now - now % PLAN_SLOT_S;
    s_pin.t0 = t0;
    s_pin.n = PLAN_SLOTS;
    /* Prix de l'horizon : HP le plus cher et HC le moins cher (tarif grille : week-end, saison). */
    float hp_max = -1.0f, hc_min = -1.0f;
    for (int k = 0; k < PLAN_SLOTS; k++) {
        int64_t ts = t0 + (int64_t)k * PLAN_SLOT_S;
        hist_pred_t p;
        history_predict((uint32_t)ts, &p);
        s_pin.pv_wh[k] = isfinite(p.pv) ? p.pv : 0.0f;
        s_pin.base_wh[k] = isfinite(p.base) ? p.base : 0.0f;
        s_pin.hc[k] = tariff_is_hc(tf, local_minute(ts + PLAN_SLOT_S / 2));
        float price = config_price_at(tf, ts + PLAN_SLOT_S / 2);
        s_pin.price[k] = price;
        if (s_pin.hc[k]) {
            hc_min = (hc_min < 0.0f || price < hc_min) ? price : hc_min;
        } else {
            hp_max = (price > hp_max) ? price : hp_max;
        }
    }
    bool soc_ok = app_state_usable(st, DEV_ZENDURE, &st->soc);
    s_pin.soc = soc_ok ? st->soc.value : s_cfg.batt.soc_min;
    s_pin.soc_min = s_cfg.batt.soc_min;
    s_pin.soc_max = s_cfg.batt.soc_max;
    s_pin.capa_kwh = s_cfg.batt.capa_kwh;
    s_pin.eta = history_batt_eta(s_cfg.batt.eta, NULL);    /* appris, sinon Paramètres */
    s_pin.wear_eur_kwh = stats_wear_cost(s_cfg.batt.price_eur, s_cfg.batt.cycles, s_cfg.batt.capa_kwh);
    s_pin.price_hp = hp_max >= 0.0f ? hp_max : tariff_mean_price(tf, false);
    s_pin.price_hc = hc_min >= 0.0f ? hc_min : tariff_mean_price(tf, true);
    s_pin.price_sell = tf->price_sell;
    s_pin.e_ce_wh = wh_remaining(&s_reg.wh);
    s_pin.shower_at = next_shower;
    planner_compute(&s_pin, &s_plan);
    s_plan_valid = true;
    planner_arbitrage(&s_pin, s_cfg.batt.p_charge_max, s_cfg.batt.p_discharge_max, &s_arb);
    if (s_cfg.batt.arbitrage && s_arb.grid_wh > 0.0f) {
        ESP_LOGI(TAG, "arbitrage : %.0f Wh à prendre en HC (SOC cible %.0f %%), gain %.3f €/kWh, %.2f € attendus",
                 (double)s_arb.grid_wh, (double)s_arb.soc_target, (double)s_arb.gain_kwh, (double)s_arb.gain_eur);
    }
    ESP_LOGI(TAG, "plan : S %.0f Wh, B %.0f Wh, E_ce %.0f Wh, mode %s, %s d'abord", s_plan.S_wh, s_plan.B_wh,
             s_plan.E_ce_wh, planner_mode_name(s_plan.mode), s_plan.ce_first ? "chauffe-eau" : "batterie");
}

static void compute_night(int64_t now, const tariff_t *tf, int64_t next_shower)
{
    wh_window_t w[MAX_HC_WINDOWS];
    int n = hc_windows(tf, now, next_shower, w, MAX_HC_WINDOWS);
    float s_ce = wh_s_ce(&s_rcfg.wh, s_plan.S_shower_wh, s_plan.B_wh);
    if (wh_night_plan(&s_rcfg.wh, &s_reg.wh, w, n, next_shower, s_ce, &s_night)) {
        ESP_LOGI(TAG, "plan de nuit : %.0f Wh de %02u:%02u à %02u:%02u%s", s_night.e_target_wh,
                 local_minute(s_night.start) / 60, local_minute(s_night.start) % 60,
                 local_minute(s_night.end) / 60, local_minute(s_night.end) % 60,
                 s_night.full ? " (complète)" : "");
    } else {
        ESP_LOGI(TAG, "pas de chauffe de nuit (soleil prévu suffisant ou ballon plein)");
    }
}

static void load_reg_cfg(void)
{
    s_rcfg = (reg_cfg_t){
        .p_charge_max = s_cfg.batt.p_charge_max,
        .p_discharge_max = s_cfg.batt.p_discharge_max,
        .soc_min = s_cfg.batt.soc_min,
        .soc_max = s_cfg.batt.soc_max,
        .solar_on_w = s_cfg.wh.solar_on_w,
        .min_on_s = (uint32_t)s_cfg.wh.min_on_min * 60,
        .min_off_s = (uint32_t)s_cfg.wh.min_off_min * 60,
        .wh = {
            .power_w = s_cfg.wh.power_w,
            .full_heat_min = s_cfg.wh.full_heat_min,
            .forecast_margin = s_cfg.wh.forecast_margin,
            .start_margin_min = s_cfg.wh.start_margin_min,
            .catchup_hp = s_cfg.wh.catchup_hp,
        },
    };
}

/* ---------- Journal ---------- */

static void log_push(const ctl_log_t *e)
{
    s_log[s_log_head] = *e;
    s_log_head = (s_log_head + 1) % CONTROL_LOG_LEN;
    if (s_log_count < CONTROL_LOG_LEN) {
        s_log_count++;
    }
}

static int16_t clamp16(float v)
{
    return (int16_t)(v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : lroundf(v)));
}

/* Une ligne si la décision change, sinon toutes les 5 min. */
static void maybe_log(int64_t now, const reg_input_t *in, const reg_output_t *o, bool observer)
{
    static ctl_log_t last;
    static bool have;
    ctl_log_t e = {
        .ts = (uint32_t)now,
        .p_grid = clamp16(in->p_grid),
        .p_batt = clamp16(in->p_batt),
        .batt_sp = clamp16(o->batt_sp),
        .avail = clamp16(o->avail_w),
        .soc = in->batt_ok ? (uint8_t)lroundf(in->soc) : SOC_UNKNOWN,
        .flags = (o->relay ? CTL_F_RELAY : 0) | (observer ? CTL_F_OBSERVER : 0) | (in->hc ? CTL_F_HC : 0) |
                 (s_reg.wh.tank_full ? CTL_F_FULL : 0) | (!in->grid_ok ? CTL_F_FALLBACK : 0),
        .ce_reason = o->ce_reason,
        /* L'attente entre deux écritures alterne avec la raison réelle : pas une décision à journaliser. */
        .batt_reason = (o->batt_reason == RB_SETTLE && have) ? last.batt_reason : o->batt_reason,
        .limits = o->batt_reason == RB_SETTLE && have ? last.limits : o->batt_limits,
        .mode = (uint8_t)in->mode,
    };
    bool changed = !have || e.flags != last.flags || e.ce_reason != last.ce_reason ||
                   e.batt_reason != last.batt_reason || e.mode != last.mode ||
                   (abs(e.batt_sp - last.batt_sp) >= LOG_SP_STEP_W && now - (int64_t)last.ts >= LOG_SP_GAP_S) ||
                   now - (int64_t)last.ts >= LOG_HEARTBEAT_S;
    if (!changed) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    log_push(&e);
    xSemaphoreGive(s_mtx);
    last = e;
    have = true;
    ESP_LOGI(TAG, "%s | CE %s%s | batt %s %+d W | réseau %+d W, SOC %d %%",
             observer ? "observateur" : "pilotage", o->relay ? "ON " : "OFF ",
             regulator_ce_reason_name(o->ce_reason), regulator_batt_reason_name(o->batt_reason), e.batt_sp,
             e.p_grid, e.soc == SOC_UNKNOWN ? -1 : e.soc);
}

/* ---------- Boucle ---------- */

static void regul_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    config_get(&s_cfg);
    load_reg_cfg();
    int64_t now = time(NULL);
    regulator_init(&s_reg, &s_rcfg, now);
    wh_load();
    int last_plan_hour = -1;
    int last_morning_yday = -1;
    bool was_hc = false;
    uint32_t last_step_ms = 0;
    app_state_listen_samples(xTaskGetCurrentTaskHandle());
    int64_t night_cycle = 0;

    for (;;) {
        /* Un pas à chaque mesure du Shelly ou du SolarFlow, et au moins toutes les 5 s. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(REGUL_PERIOD_MS));
        esp_task_wdt_reset();
        uint32_t now_ms = app_now_ms();
        if (last_step_ms != 0 && now_ms - last_step_ms < REGUL_MIN_STEP_MS) {
            continue;
        }

        static app_state_t st;
        app_state_get(&st);
        config_get(&s_cfg);
        load_reg_cfg();
        now = time(NULL);
        bool time_ok = st.time_synced && now > 1700000000;

        tariff_t tf;
        config_tariff(&s_cfg, &tf);
        uint16_t minute = time_ok ? local_minute(now) : 0;
        bool hc = time_ok && tariff_is_hc(&tf, minute);
        int64_t last_shower = 0, next_shower = 0;
        if (time_ok) {
            showers(now, s_cfg.wh.shower_min, &last_shower, &next_shower);
            struct tm tm;
            time_t tt = (time_t)now;
            localtime_r(&tt, &tm);
            bool morning = minute >= PLAN_MORNING_MIN && tm.tm_yday != last_morning_yday;
            /* Aussi au début de chaque plage HC : l'arbitrage part du SOC réel de ce moment. */
            if (!s_plan_valid || tm.tm_hour != last_plan_hour || morning || (hc && !was_hc)) {
                compute_plan(now, &st, &tf, next_shower);
                last_plan_hour = tm.tm_hour;
                if (morning) {
                    last_morning_yday = tm.tm_yday;
                }
            }
            /* Plan de nuit au début de chaque plage HC (ou au premier pas pris en HC dans ce cycle). */
            if (hc && (!was_hc || night_cycle != last_shower)) {
                compute_night(now, &tf, next_shower);
                night_cycle = last_shower;
            }
        }
        was_hc = hc;

        flows_in_t fin;
        bool full;
        bool grid_ok = app_state_flows_in(&st, &fin, &full) && !st.shelly_fallback;
        reg_override_t ovr;
        int64_t ovr_until;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        ovr = s_ovr;
        ovr_until = s_ovr_until;
        xSemaphoreGive(s_mtx);

        float soc_target = s_plan_valid ? planner_soc_target(&s_plan, now, s_cfg.batt.soc_min) : s_cfg.batt.soc_max;
        reg_input_t in = {
            .cfg = &s_rcfg,
            .now = now,
            .dt_s = last_step_ms ? (float)(now_ms - last_step_ms) / 1000.0f : 2.0f,
            .time_ok = time_ok,
            .hc = hc,
            .grid_ok = grid_ok,
            .p_grid = fin.p_grid,
            .batt_ok = app_state_usable(&st, DEV_ZENDURE, &st.p_batt) && app_state_usable(&st, DEV_ZENDURE, &st.soc),
            .p_batt = st.p_batt.value,
            .soc = st.soc.value,
            .ce_ok = app_state_usable(&st, DEV_TONGOU, &st.p_ce),
            .p_ce = st.p_ce.value,
            .relay_on = st.ce_relay_on,
            .mode = s_plan_valid ? s_plan.mode : CE_HC_ONLY,     /* §10 : HC uniquement sans plan */
            .soc_target = soc_target,
            .night = s_night,
            .last_shower = last_shower,
            .next_shower = next_shower,
            .override = ovr,
            .override_until = ovr_until,
        };
        if (s_cfg.batt.arbitrage && s_plan_valid && now >= s_plan.t0) {
            int64_t k = (now - s_plan.t0) / PLAN_SLOT_S;
            if (k < PLAN_SLOTS) {
                in.arb_charge_w = s_arb.charge_wh[k] * 3600.0f / PLAN_SLOT_S;
                in.arb_soc_target = s_arb.soc_target;
            }
        }
        last_step_ms = now_ms;
        bool observer = s_cfg.sys.observer;
        in.batt_hold =!observer && !control_act_settled(now_ms, st.p_grid.ts_ms, st.p_batt.ts_ms);
        reg_output_t out;
        regulator_step(&in, &s_reg, &out);
        if (out.tank_full_event) {
            ESP_LOGI(TAG, "coupure du thermostat : ballon plein (E_full %.0f Wh)", s_reg.wh.e_full_wh);
            wh_save();
        }
        if (out.new_cycle_event) {
            ESP_LOGI(TAG, "heure des douches : nouveau cycle du chauffe-eau");
        }
        if (out.catchup_blocked) {
            ESP_LOGW(TAG, "rattrapage interdit en HP : eau peut-être tiède ce soir");
        }

        maybe_log(now, &in, &out, observer);
        if (time_ok && !observer) {
            control_act_submit(out.batt_sp, out.relay);
        }

        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_status.running = time_ok;
        s_status.observer = observer;
        s_status.plan_valid = s_plan_valid;
        s_status.plan = s_plan;
        s_status.arb = s_arb;
        s_status.arb_enabled = s_cfg.batt.arbitrage;
        s_status.night = s_night;
        s_status.out = out;
        s_status.soc_target = soc_target;
        s_status.last_shower = last_shower;
        s_status.next_shower = next_shower;
        s_status.e_full_wh = s_reg.wh.e_full_wh;
        s_status.e_heated_wh = s_reg.wh.e_heated_wh;
        s_status.tank_full = s_reg.wh.tank_full;
        s_status.last_full = s_reg.wh.last_full;
        s_status.override = (uint8_t)(now < ovr_until ? ovr : REG_OVR_AUTO);
        s_status.override_until = ovr_until;
        s_status.updated = now;
        xSemaphoreGive(s_mtx);
    }
}

/* ---------- API ---------- */

esp_err_t control_start(void)
{
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    if (s_mtx == NULL) {
        return ESP_FAIL;
    }
    esp_err_t err = control_act_start();
    if (err != ESP_OK) {
        return err;
    }
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(regul_task, "task_regul", REGUL_STACK, NULL, REGUL_PRIO,
                                                   s_task_stack, &s_task_buf, REGUL_CORE);
    return h != NULL ? ESP_OK : ESP_FAIL;
}

void control_status(control_status_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mtx);
}

int control_log_read(ctl_log_t *out, int max)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int n = s_log_count < max ? s_log_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_log[(s_log_head - 1 - i + CONTROL_LOG_LEN) % CONTROL_LOG_LEN];
    }
    xSemaphoreGive(s_mtx);
    return n;
}

esp_err_t control_override(reg_override_t mode, uint16_t minutes)
{
    if (mode > REG_OVR_OFF || (mode != REG_OVR_AUTO && (minutes == 0 || minutes > 24 * 60))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_ovr = mode;
    s_ovr_until = mode == REG_OVR_AUTO ? 0 : (int64_t)time(NULL) + (int64_t)minutes * 60;
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "forçage du chauffe-eau : %s pour %u min",
             mode == REG_OVR_ON ? "ON" : mode == REG_OVR_OFF ? "OFF" : "AUTO", minutes);
    return ESP_OK;
}
