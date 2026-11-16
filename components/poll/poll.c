#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "app_state.h"
#include "config.h"
#include "sanity.h"
#include "drv_shelly.h"
#include "drv_envoy.h"
#include "drv_zendure.h"
#include "drv_tongou.h"
#include "history.h"
#include "poll.h"

static const char *TAG = "poll";

#define GRID_STACK              6144    /* history_on_sample : localtime_r + flows_split */
#define POLL_STACK              5120
#define POLL_PRIO               5
#define POLL_CORE               0
#define POLL_TICK_MS            100
#define ENVOY_PRIO              4
#define ENVOY_CORE              1
#define STALE_PERIODS           3
#define SHELLY_FALLBACK_MS      30000
#define ZENDURE_TOLERATED_ERR   3       /* timeouts signalés par d'autres utilisateurs (§8.3) */
#define PV_NEG_TOLERANCE_W      50.0f
#define BACKOFF_AFTER_ERR       3       /* échecs consécutifs avant d'espacer les essais */
#define BACKOFF_MS              30000
#define ZENDURE_STALE_MS        15000
#define SHELLY_STALE_MS         8000

static StaticTask_t s_grid_buf;
static StackType_t s_grid_stack[GRID_STACK];
static StaticTask_t s_task_buf;
static StackType_t s_task_stack[POLL_STACK];
static StaticTask_t s_envoy_buf;
static StackType_t s_envoy_stack[POLL_STACK];
static cfg_t s_cfg_grid;    /* copie propre à chaque tâche */
static cfg_t s_cfg_slow;
static cfg_t s_cfg_envoy;

typedef struct {
    uint32_t period_ms;
    uint8_t  tolerated_err;             /* échecs consécutifs avant valid = false */
    uint32_t stale_min_ms;              /* âge minimal avant « périmé » (0 = 3 périodes) */
    uint32_t next_ms;
} sched_t;

static sched_t s_sched[DEV_COUNT];

static bool configured(const cfg_t *cfg, dev_id_t id)
{
    switch (id) {
    case DEV_SHELLY:  return cfg->dev.shelly_ip[0] != '\0';
    case DEV_ENVOY:   return cfg->dev.envoy_ip[0] != '\0';
    case DEV_ZENDURE: return cfg->dev.zendure_ip[0] != '\0';
    case DEV_TONGOU:  return cfg->dev.tongou_ip[0] != '\0';
    default:          return false;
    }
}

static const char *dev_name(dev_id_t id)
{
    static const char *names[DEV_COUNT] = { "Shelly", "Envoy", "Zendure", "Tongou" };
    return names[id];
}

/* Mesure acceptée seulement dans ses bornes de plausibilité (§7). */
static void set_meas(meas_t *m, float v, qty_t q, float kwc, uint32_t now, const char *what)
{
    if (sanity_ok(q, v, kwc)) {
        m->value = v;
        m->ts_ms = now;
        m->valid = true;
    } else {
        m->valid = false;
        ESP_LOGW(TAG, "%s hors bornes : %.0f, mesure rejetée", what, v);
    }
}

static void invalidate(app_state_t *st, dev_id_t id)
{
    switch (id) {
    case DEV_SHELLY:
        st->p_grid.valid = false;
        break;
    case DEV_ENVOY:
        st->p_pv.valid = false;
        break;
    case DEV_ZENDURE:
        st->p_batt.valid = false;
        st->soc.valid = false;
        break;
    case DEV_TONGOU:
        st->p_ce.valid = false;
        break;
    default:
        break;
    }
}

/* Après chaque lecture : compteurs d'erreurs, invalidation, état périmé. */
static void account(app_state_t *st, dev_id_t id, esp_err_t err, uint32_t now)
{
    dev_status_t *d = &st->dev[id];
    if (err == ESP_OK) {
        if (d->consecutive_err > 0 || !d->ever_ok) {
            ESP_LOGI(TAG, "%s : lecture OK", dev_name(id));
        }
        d->ever_ok = true;
        d->last_ok_ms = now;
        d->consecutive_err = 0;
        return;
    }
    d->err_count++;
    d->last_err = err;
    if (d->consecutive_err < UINT8_MAX) {
        d->consecutive_err++;
    }
    if (d->consecutive_err == 1) {
        ESP_LOGW(TAG, "%s : échec de lecture (%s)", dev_name(id), esp_err_to_name(err));
    }
    if (d->consecutive_err >= s_sched[id].tolerated_err) {
        invalidate(st, id);
    }
}

static void poll_device(const cfg_t *cfg, dev_id_t id, uint32_t now)
{
    esp_err_t err;
    switch (id) {
    case DEV_SHELLY: {
        shelly_reading_t r;
        err = drv_shelly_poll(cfg, &r);
        app_state_t *st = app_state_lock();
        if (err == ESP_OK) {
            set_meas(&st->p_grid, r.total_w, QTY_GRID, 0.0f, now, "p_grid");
            memcpy(st->p_grid_phase, r.phase_w, sizeof(st->p_grid_phase));
            st->grid_mono = r.mono;
        }
        account(st, id, err, now);
        app_state_unlock();
        /* Intégration des énergies au pas du Shelly (§9.9). */
        app_state_t copy;
        app_state_get(&copy);
        history_on_sample(&copy);
        app_state_notify_sample();
        break;
    }
    case DEV_ENVOY: {
        envoy_reading_t r;
        err = drv_envoy_poll(cfg, &r);
        app_state_t *st = app_state_lock();
        if (err == ESP_OK) {
            /* Légère valeur négative (consommation propre du compteur) ramenée à 0 ;
             * au-delà, la borne de plausibilité la rejette. */
            float w = (r.w_now < 0.0f && r.w_now > -PV_NEG_TOLERANCE_W) ? 0.0f : r.w_now;
            set_meas(&st->p_pv, w, QTY_PV, cfg->pv.kwc, now, "p_pv");
            st->pv_from_eim = r.from_eim;
            st->pv_wh_valid = r.wh_valid && st->p_pv.valid;
            st->pv_wh_lifetime = r.wh_lifetime;
        }
        account(st, id, err, now);
        app_state_unlock();
        break;
    }
    case DEV_ZENDURE: {
        zendure_reading_t r;
        err = drv_zendure_poll(cfg, &r);
        app_state_t *st = app_state_lock();
        if (err == ESP_OK) {
            set_meas(&st->p_batt, r.p_batt, QTY_BATT, 0.0f, now, "p_batt");
            set_meas(&st->soc, r.soc, QTY_SOC, 0.0f, now, "soc");
            st->zendure.ac_mode = r.ac_mode;
            st->zendure.input_limit_w = r.input_limit_w;
            st->zendure.output_limit_w = r.output_limit_w;
            st->zendure.soc_set_pm = r.soc_set_pm;
            st->zendure.min_soc_pm = r.min_soc_pm;
        }
        account(st, id, err, now);
        app_state_unlock();
        app_state_notify_sample();
        break;
    }
    case DEV_TONGOU: {
        tongou_reading_t r;
        err = drv_tongou_poll(cfg, &r);
        app_state_t *st = app_state_lock();
        if (err == ESP_OK) {
            set_meas(&st->p_ce, r.power_w, QTY_CE, 0.0f, now, "p_ce");
            st->ce_relay_on = r.relay_on;
        }
        account(st, id, err, now);
        app_state_unlock();
        break;
    }
    default:
        break;
    }
}

/* État périmé, appareils non configurés, mode repli du Shelly. */
static void refresh_status(const cfg_t *cfg, uint32_t now)
{
    app_state_t *st = app_state_lock();
    for (int id = 0; id < DEV_COUNT; id++) {
        dev_status_t *d = &st->dev[id];
        bool cfgd = configured(cfg, (dev_id_t)id);
        if (!cfgd && d->configured) {
            memset(d, 0, sizeof(*d));
            invalidate(st, (dev_id_t)id);
        }
        d->configured = cfgd;
        uint32_t max_age = STALE_PERIODS * s_sched[id].period_ms;
        if (max_age < s_sched[id].stale_min_ms) {
            max_age = s_sched[id].stale_min_ms;
        }
        d->stale = cfgd && (!d->ever_ok || now - d->last_ok_ms > max_age);
    }
    const dev_status_t *sh = &st->dev[DEV_SHELLY];
    bool fallback = !sh->ever_ok ? now > SHELLY_FALLBACK_MS : now - sh->last_ok_ms > SHELLY_FALLBACK_MS;
    if (fallback != st->shelly_fallback) {
        if (fallback) {
            ESP_LOGW(TAG, "Shelly invalide depuis plus de 30 s : mode repli");
        } else {
            ESP_LOGI(TAG, "Shelly de nouveau valide : fin du mode repli");
        }
    }
    st->shelly_fallback = fallback;
    app_state_unlock();
}

static uint8_t consecutive_errors(dev_id_t id)
{
    app_state_t *st = app_state_lock();
    uint8_t n = st->dev[id].consecutive_err;
    app_state_unlock();
    return n;
}

/* Lectures séquentielles des appareils [first, last] de cette tâche. Un appareil qui échoue
 * plusieurs fois de suite est interrogé moins souvent, pour ne pas retarder les autres. */
static void run_devices(const cfg_t *cfg, dev_id_t first, dev_id_t last)
{
    for (int id = first; id <= last; id++) {
        uint32_t now = app_now_ms();
        if (configured(cfg, (dev_id_t)id) && (int32_t)(now - s_sched[id].next_ms) >= 0) {
            poll_device(cfg, (dev_id_t)id, now);
            uint32_t period = s_sched[id].period_ms;
            if (id != DEV_SHELLY && consecutive_errors((dev_id_t)id) >= BACKOFF_AFTER_ERR && period < BACKOFF_MS) {
                period = BACKOFF_MS;
            }
            s_sched[id].next_ms = now + period;
            esp_task_wdt_reset();
        }
    }
}

static bool wifi_up(void)
{
    app_state_t *st = app_state_lock();
    bool up = st->wifi_connected;
    app_state_unlock();
    return up;
}

/* task_grid : le Shelly seul, capteur maître. Jamais retardé par un appareil lent (Envoy en TLS,
 * appareil injoignable) ; c'est aussi elle qui tient l'état périmé et le mode repli à jour. */
static void grid_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        config_get(&s_cfg_grid);
        s_sched[DEV_SHELLY].period_ms = s_cfg_grid.sys.poll_ms;
        if (wifi_up()) {
            run_devices(&s_cfg_grid, DEV_SHELLY, DEV_SHELLY);
        }
        refresh_status(&s_cfg_grid, app_now_ms());
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(POLL_TICK_MS));
    }
}

/* Tâche de lecture d'une plage d'appareils, chacune avec sa copie de la config. */
typedef struct {
    dev_id_t first;
    dev_id_t last;
    cfg_t   *cfg;
} poll_group_t;

static void poll_task(void *arg)
{
    const poll_group_t *g = arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        if (wifi_up()) {
            config_get(g->cfg);
            run_devices(g->cfg, g->first, g->last);
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(POLL_TICK_MS));
    }
}

/* task_poll : Zendure et Tongou (HTTP local, rapides). task_envoy : Envoy seul (HTTPS, réponse
 * en plusieurs secondes) pour qu'il ne périme pas les lectures du Zendure. */
static const poll_group_t s_local = { DEV_ZENDURE, DEV_TONGOU, &s_cfg_slow };
static const poll_group_t s_envoy = { DEV_ENVOY, DEV_ENVOY, &s_cfg_envoy };

esp_err_t poll_start(void)
{
    /* Périmé après 3 périodes, mais pas avant 8 s : une lecture qui réussit au second essai (3 s de
     * délai dépassé) n'est pas une perte du compteur, même avec une période de 1 s. */
    s_sched[DEV_SHELLY] = (sched_t){ .period_ms = 2000, .tolerated_err = 1, .stale_min_ms = SHELLY_STALE_MS };
    s_sched[DEV_ENVOY] = (sched_t){ .period_ms = POLL_ENVOY_MS, .tolerated_err = 1 };
    /* Lu toutes les 2 s, mais périmé seulement après 15 s : une lecture lente ne coupe pas la régulation. */
    s_sched[DEV_ZENDURE] = (sched_t){ .period_ms = POLL_ZENDURE_MS, .tolerated_err = ZENDURE_TOLERATED_ERR,
                                      .stale_min_ms = ZENDURE_STALE_MS };
    s_sched[DEV_TONGOU] = (sched_t){ .period_ms = POLL_TONGOU_MS, .tolerated_err = 1 };

    TaskHandle_t g = xTaskCreateStaticPinnedToCore(grid_task, "task_grid", GRID_STACK, NULL, POLL_PRIO,
                                                   s_grid_stack, &s_grid_buf, POLL_CORE);
    TaskHandle_t p = xTaskCreateStaticPinnedToCore(poll_task, "task_poll", POLL_STACK, (void *)&s_local,
                                                   POLL_PRIO, s_task_stack, &s_task_buf, POLL_CORE);
    /* La négociation TLS de l'Envoy (toutes les 10 s) retardait httpd de 1 à 2 s sur le cœur 0 :
     * cœur 1, priorité sous httpd. */
    TaskHandle_t e = xTaskCreateStaticPinnedToCore(poll_task, "task_envoy", POLL_STACK, (void *)&s_envoy,
                                                   ENVOY_PRIO, s_envoy_stack, &s_envoy_buf, ENVOY_CORE);
    return (g != NULL && p != NULL && e != NULL) ? ESP_OK : ESP_FAIL;
}
