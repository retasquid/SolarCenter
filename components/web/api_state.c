#include <math.h>
#include <time.h>
#include "app_state.h"
#include "config.h"
#include "flows.h"
#include "web_priv.h"

static const char *k_dev_keys[DEV_COUNT] = { "shelly", "envoy", "zendure", "tongou" };

static void add_w(cJSON *o, const char *k, float w)
{
    cJSON_AddNumberToObject(o, k, (double)lroundf(w));
}

/* Mesure utilisable : lue, dans ses bornes, et pas périmée. */
static bool usable(const meas_t *m, const dev_status_t *d)
{
    return m->valid && d->configured && !d->stale;
}

static cJSON *add_meas(cJSON *r, const char *k, const meas_t *m, const dev_status_t *d, uint32_t now)
{
    cJSON *o = cJSON_AddObjectToObject(r, k);
    if (o == NULL) {
        return NULL;
    }
    add_w(o, "w", m->value);
    cJSON_AddBoolToObject(o, "ok", usable(m, d));
    cJSON_AddBoolToObject(o, "configured", d->configured);
    if (m->ts_ms != 0) {
        cJSON_AddNumberToObject(o, "age_s", (now - m->ts_ms) / 1000);
    }
    return o;
}

cJSON *state_to_json(void)
{
    app_state_t st;
    app_state_get(&st);
    uint32_t now = app_now_ms();
    const dev_status_t *dv = st.dev;

    cJSON *r = cJSON_CreateObject();
    if (r == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(r, "time", (double)time(NULL));
    cJSON_AddBoolToObject(r, "time_synced", st.time_synced);
    cJSON_AddBoolToObject(r, "fallback", st.shelly_fallback);

    cJSON *grid = add_meas(r, "grid", &st.p_grid, &dv[DEV_SHELLY], now);
    if (grid != NULL) {
        cJSON *ph = cJSON_AddArrayToObject(grid, "phases");
        for (int i = 0; i < 3 && ph != NULL; i++) {
            cJSON_AddItemToArray(ph, cJSON_CreateNumber((double)lroundf(st.p_grid_phase[i])));
        }
    }
    cJSON *pv = add_meas(r, "pv", &st.p_pv, &dv[DEV_ENVOY], now);
    if (pv != NULL) {
        cJSON_AddBoolToObject(pv, "eim", st.pv_from_eim);
    }
    cJSON *batt = add_meas(r, "batt", &st.p_batt, &dv[DEV_ZENDURE], now);
    if (batt != NULL) {
        cJSON_AddNumberToObject(batt, "soc", (double)lroundf(st.soc.value));
        cJSON_AddBoolToObject(batt, "soc_ok", usable(&st.soc, &dv[DEV_ZENDURE]));
        cJSON_AddNumberToObject(batt, "ac_mode", st.zendure.ac_mode);
        add_w(batt, "input_limit", st.zendure.input_limit_w);
        add_w(batt, "output_limit", st.zendure.output_limit_w);
        cJSON_AddNumberToObject(batt, "soc_set", st.zendure.soc_set_pm / 10);
        cJSON_AddNumberToObject(batt, "min_soc", st.zendure.min_soc_pm / 10);
    }
    cJSON *ce = add_meas(r, "ce", &st.p_ce, &dv[DEV_TONGOU], now);
    if (ce != NULL) {
        cJSON_AddBoolToObject(ce, "relay", st.ce_relay_on);
    }

    /* Flux : le Shelly est indispensable ; un appareil absent de la config compte pour 0 W. */
    bool grid_ok = usable(&st.p_grid, &dv[DEV_SHELLY]);
    bool pv_ok = !dv[DEV_ENVOY].configured || usable(&st.p_pv, &dv[DEV_ENVOY]);
    bool batt_ok = !dv[DEV_ZENDURE].configured || usable(&st.p_batt, &dv[DEV_ZENDURE]);
    bool ce_ok = !dv[DEV_TONGOU].configured || usable(&st.p_ce, &dv[DEV_TONGOU]);
    const flows_in_t in = {
        .p_grid = grid_ok ? st.p_grid.value : 0.0f,
        .p_pv = usable(&st.p_pv, &dv[DEV_ENVOY]) ? st.p_pv.value : 0.0f,
        .p_batt = usable(&st.p_batt, &dv[DEV_ZENDURE]) ? st.p_batt.value : 0.0f,
        .p_ce = usable(&st.p_ce, &dv[DEV_TONGOU]) ? st.p_ce.value : 0.0f,
    };
    flows_t f;
    flows_split(&in, &f);
    bool flows_ok = grid_ok && pv_ok && batt_ok && ce_ok;

    cJSON *home = cJSON_AddObjectToObject(r, "home");
    if (home != NULL) {
        add_w(home, "w", f.p_home);
        add_w(home, "base", f.p_base);
        cJSON_AddBoolToObject(home, "ok", flows_ok);
    }
    cJSON *fl = cJSON_AddObjectToObject(r, "flows");
    if (fl != NULL) {
        cJSON_AddBoolToObject(fl, "ok", flows_ok);
        add_w(fl, "pv_home", f.pv_home);
        add_w(fl, "pv_batt", f.pv_batt);
        add_w(fl, "pv_grid", f.pv_grid);
        add_w(fl, "grid_home", f.grid_home);
        add_w(fl, "grid_batt", f.grid_batt);
        add_w(fl, "batt_home", f.batt_home);
        add_w(fl, "batt_grid", f.batt_grid);
        add_w(fl, "home_ce", f.home_ce);
        add_w(fl, "ce_local", f.ce_local);
    }

    cJSON *devs = cJSON_AddObjectToObject(r, "dev");
    for (int i = 0; i < DEV_COUNT && devs != NULL; i++) {
        cJSON *d = cJSON_AddObjectToObject(devs, k_dev_keys[i]);
        if (d == NULL) {
            break;
        }
        cJSON_AddBoolToObject(d, "configured", dv[i].configured);
        cJSON_AddBoolToObject(d, "ok", dv[i].configured && dv[i].ever_ok && !dv[i].stale &&
                                       dv[i].consecutive_err == 0);
        cJSON_AddBoolToObject(d, "stale", dv[i].stale);
        if (dv[i].ever_ok) {
            cJSON_AddNumberToObject(d, "age_s", (now - dv[i].last_ok_ms) / 1000);
        }
        cJSON_AddNumberToObject(d, "errors", dv[i].err_count);
        cJSON_AddNumberToObject(d, "consecutive", dv[i].consecutive_err);
        if (dv[i].err_count > 0) {
            cJSON_AddStringToObject(d, "last_err", esp_err_to_name(dv[i].last_err));
        }
    }
    return r;
}
