#include <math.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_check.h"
#include "config.h"
#include "forecast.h"
#include "history.h"
#include "stats.h"
#include "profile.h"
#include "web_priv.h"

static const char *TAG = "web";

#define SLOTS_24H           48
#define SLOTS_7D            336
#define FORECAST_SLOTS      96      /* aujourd'hui et demain */
#define MAE_DAYS            7
#define MAE_MIN_COVER       80      /* % */
#define RATIO_SURPLUS_DAYS  30      /* en dessous, ratio surplus/PV par défaut */
#define RATIO_SURPLUS_DEF   0.5f
#define ROI_SPREAD          0.2f    /* fourchette ±20 % tant que < 12 mois */

/* Tampons statiques : tous les gestionnaires s'exécutent dans l'unique tâche httpd. */
static jw_t s_jw;
static cfg_t s_cfg;
static char s_body[128];

static bool query_param(httpd_req_t *req, const char *key, char *val, size_t cap)
{
    char q[64];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, key, val, cap) == ESP_OK;
}

static uint32_t slot_start(int64_t t)
{
    struct tm tm;
    time_t tt = (time_t)t;
    localtime_r(&tt, &tm);
    return (uint32_t)(t - (tm.tm_min % 30) * 60 - tm.tm_sec);
}

static bool time_ok(void)
{
    return time(NULL) > 1700000000;
}

/* ---------- GET /api/history?range=24h|7d ---------- */

typedef struct {
    jw_t *w;
    bool  first;
} rows_ctx_t;

static bool emit_slot(const hist_slot_t *s, void *arg)
{
    rows_ctx_t *c = arg;
    jw_printf(c->w, "%s[%lu,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,", c->first ? "" : ",", (unsigned long)s->ts,
              (double)s->e_pv, (double)s->e_home, (double)s->e_ce, (double)s->e_import, (double)s->e_export,
              (double)s->e_batt_ch, (double)s->e_batt_dis);
    if (s->soc <= 100) {
        jw_printf(c->w, "%u,", s->soc);
    } else {
        jw_printf(c->w, "null,");
    }
    jw_printf(c->w, "%u,", s->cov_full);
    jw_num(c->w, s->pv_pred, 0);
    jw_printf(c->w, ",");
    jw_num(c->w, s->base_pred, 0);
    jw_printf(c->w, "]");
    c->first = false;
    return true;
}

static esp_err_t history_get(httpd_req_t *req)
{
    char range[8] = "24h";
    query_param(req, "range", range, sizeof(range));
    int n;
    if (strcmp(range, "24h") == 0) {
        n = SLOTS_24H;
    } else if (strcmp(range, "7d") == 0) {
        n = SLOTS_7D;
    } else {
        return web_send_error(req, WEB_400, "range attendu : 24h ou 7d", "range");
    }
    if (!time_ok()) {
        return web_send_error(req, "503 Service Unavailable", "heure non synchronisée", NULL);
    }
    uint32_t last = slot_start(time(NULL)) - HIST_SLOT_S;     /* dernier créneau clos */
    uint32_t first = last - (uint32_t)(n - 1) * HIST_SLOT_S;

    jw_begin(&s_jw, req);
    jw_printf(&s_jw, "{\"range\":\"%s\",\"slot_s\":%d,\"cols\":[\"t\",\"pv\",\"home\",\"ce\",\"imp\",\"exp\","
              "\"ch\",\"dis\",\"soc\",\"cov\",\"pv_pred\",\"base_pred\"],\"rows\":[", range, HIST_SLOT_S);
    rows_ctx_t c = { .w = &s_jw, .first = true };
    history_foreach_slot(first, last, emit_slot, &c);
    jw_printf(&s_jw, "]}");
    return jw_end(&s_jw);
}

/* ---------- GET /api/forecast ---------- */

typedef struct {
    float pv_err, base_err;
    int   pv_n, base_n;
} mae_ctx_t;

static bool mae_slot(const hist_slot_t *s, void *arg)
{
    mae_ctx_t *m = arg;
    if (s->cov_full < MAE_MIN_COVER) {
        return true;
    }
    if (isfinite(s->pv_pred) && (s->pv_pred > 0.0f || s->e_pv > 0.0f)) {
        m->pv_err += fabsf(s->pv_pred - s->e_pv);
        m->pv_n++;
    }
    if (isfinite(s->base_pred)) {
        m->base_err += fabsf(s->base_pred - (s->e_home - s->e_ce));
        m->base_n++;
    }
    return true;
}

static void emit_model(jw_t *w, const char *name, const rls_t *r, float mae, int n)
{
    jw_printf(w, "\"%s\":{\"samples\":%lu,\"ready\":%s,\"resets\":%lu,\"theta\":[", name,
              (unsigned long)r->samples, rls_ready(r) ? "true" : "false", (unsigned long)r->resets);
    for (int i = 0; i < r->n; i++) {
        jw_printf(w, "%s", i ? "," : "");
        jw_num(w, r->theta[i], 5);
    }
    jw_printf(w, "],\"mae7\":");
    jw_num(w, n > 0 ? mae / (float)n : NAN, 0);
    jw_printf(w, ",\"n7\":%d}", n);
}

static esp_err_t forecast_get_handler(httpd_req_t *req)
{
    if (!time_ok()) {
        return web_send_error(req, "503 Service Unavailable", "heure non synchronisée", NULL);
    }
    static forecast_t fc;
    forecast_get(&fc);
    config_get(&s_cfg);
    int64_t now = time(NULL);

    /* Créneaux depuis minuit local aujourd'hui. */
    struct tm tm;
    time_t tt = (time_t)now;
    localtime_r(&tt, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    uint32_t midnight = (uint32_t)mktime(&tm);

    mae_ctx_t mae = { 0 };
    uint32_t last = slot_start(now) - HIST_SLOT_S;
    history_foreach_slot(last - (MAE_DAYS * 48 - 1) * HIST_SLOT_S, last, mae_slot, &mae);
    static rls_t pv, base;
    int profile_slots = 0;
    history_model_info(&pv, &base, &profile_slots);

    jw_begin(&s_jw, req);
    jw_printf(&s_jw, "{\"ok\":%s,\"fetched\":%lld,\"coords\":%s,\"midnight\":%lu,\"slot_s\":%d,",
              fc.valid ? "true" : "false", (long long)fc.fetched,
              (s_cfg.pv.lat != 0.0f || s_cfg.pv.lon != 0.0f) ? "true" : "false",
              (unsigned long)midnight, HIST_SLOT_S);
    jw_printf(&s_jw, "\"cols\":[\"t\",\"gti\",\"temp\",\"pv_raw\",\"pv\",\"base_prof\",\"base\"],\"rows\":[");
    for (int i = 0; i < FORECAST_SLOTS; i++) {
        uint32_t ts = midnight + (uint32_t)i * HIST_SLOT_S;
        hist_pred_t p;
        history_predict(ts, &p);
        jw_printf(&s_jw, "%s[%lu,", i ? "," : "", (unsigned long)ts);
        jw_num(&s_jw, p.gti, 0);
        jw_printf(&s_jw, ",");
        jw_num(&s_jw, p.temp, 1);
        jw_printf(&s_jw, ",");
        jw_num(&s_jw, p.pv_raw, 0);
        jw_printf(&s_jw, ",");
        jw_num(&s_jw, p.pv, 0);
        jw_printf(&s_jw, ",");
        jw_num(&s_jw, p.base_prof, 0);
        jw_printf(&s_jw, ",");
        jw_num(&s_jw, p.base, 0);
        jw_printf(&s_jw, "]");
    }
    jw_printf(&s_jw, "],");
    emit_model(&s_jw, "pv", &pv, mae.pv_err, mae.pv_n);
    jw_printf(&s_jw, ",");
    emit_model(&s_jw, "base", &base, mae.base_err, mae.base_n);
    jw_printf(&s_jw, ",\"profile_slots\":%d,\"profile_total\":%d}", profile_slots, PROFILE_SLOTS * PROFILE_DAY_TYPES);
    return jw_end(&s_jw);
}

/* ---------- GET /api/stats?period=day|7d|30d|year|all ---------- */

typedef struct {
    energy_acc_t acc;
    int          days;
    int          year;      /* filtre « year », 0 = tous */
} sum_ctx_t;

static bool sum_day(const hist_day_t *d, void *arg)
{
    sum_ctx_t *c = arg;
    stats_add(&c->acc, &d->acc);
    c->days++;
    return true;
}

static bool sum_month(const hist_month_t *m, void *arg)
{
    sum_ctx_t *c = arg;
    if (c->year == 0 || (int)(m->month / 100) == c->year) {
        stats_add(&c->acc, &m->acc);
        c->days += m->days;
    }
    return true;
}

typedef struct {
    float       wear;
    int         cur_ym;                 /* AAAA·12 + (MM − 1) du mois courant */
    roi_month_t last12[12];             /* indexé par mois calendaire */
    float       old_surplus[12];        /* kWh, mois de plus de 12 mois */
    int         old_days[12];
    float       cum_net;                /* € depuis le début de l'historique */
    float       pv_kwh, surplus_kwh;
    int         total_days;
} roi_ctx_t;

static bool roi_month(const hist_month_t *m, void *arg)
{
    roi_ctx_t *c = arg;
    rates_t r;
    stats_rates(&m->acc, c->wear, &r);
    c->cum_net += r.gain_batt_net;
    float surplus = (m->acc.e_pv_batt + m->acc.e_pv_grid) / 1000.0f;
    c->pv_kwh += m->acc.e_pv / 1000.0f;
    c->surplus_kwh += surplus;
    c->total_days += m->days;

    int ym = (int)(m->month / 100) * 12 + (int)(m->month % 100) - 1;
    int mi = (int)(m->month % 100) - 1;
    if (mi < 0 || mi > 11) {
        return true;
    }
    if (c->cur_ym - ym < 12) {
        c->last12[mi].observed = true;
        c->last12[mi].gain_batt_net += r.gain_batt_net;
        c->last12[mi].days_observed += (uint8_t)m->days;
    } else {
        c->old_surplus[mi] += surplus;
        c->old_days[mi] += m->days;
    }
    return true;
}

static void emit_acc(jw_t *w, const energy_acc_t *a)
{
    const struct { const char *k; float v; } e[] = {
        { "pv", a->e_pv }, { "import", a->e_import }, { "export", a->e_export }, { "home", a->e_home },
        { "batt_ch", a->e_batt_ch }, { "batt_dis", a->e_batt_dis }, { "ce", a->e_ce },
        { "ce_local", a->e_ce_local }, { "pv_home", a->e_pv_home }, { "pv_batt", a->e_pv_batt },
        { "pv_grid", a->e_pv_grid }, { "grid_home", a->e_grid_home }, { "grid_batt", a->e_grid_batt },
        { "batt_home", a->e_batt_home },
    };
    jw_printf(w, "\"kwh\":{");
    for (size_t i = 0; i < sizeof(e) / sizeof(e[0]); i++) {
        jw_printf(w, "%s\"%s\":", i ? "," : "", e[i].k);
        jw_num(w, e[i].v / 1000.0f, 3);
    }
    jw_printf(w, "}");
}

static esp_err_t stats_get(httpd_req_t *req)
{
    char period[8] = "day";
    query_param(req, "period", period, sizeof(period));
    if (!time_ok()) {
        return web_send_error(req, "503 Service Unavailable", "heure non synchronisée", NULL);
    }
    config_get(&s_cfg);
    float wear = stats_wear_cost(s_cfg.batt.price_eur, s_cfg.batt.cycles, s_cfg.batt.capa_kwh);

    static sum_ctx_t sum;
    memset(&sum, 0, sizeof(sum));
    uint32_t today_date = 0;
    static energy_acc_t today;
    history_today(&today, &today_date);
    stats_add(&sum.acc, &today);
    sum.days = 1;
    int year = (int)(today_date / 10000);

    if (strcmp(period, "7d") == 0) {
        history_foreach_day(6, sum_day, &sum);
    } else if (strcmp(period, "30d") == 0) {
        history_foreach_day(29, sum_day, &sum);
    } else if (strcmp(period, "year") == 0) {
        sum.year = year;
        history_foreach_month(sum_month, &sum);
    } else if (strcmp(period, "all") == 0) {
        history_foreach_month(sum_month, &sum);
    } else if (strcmp(period, "day") != 0) {
        return web_send_error(req, WEB_400, "period attendu : day, 7d, 30d, year ou all", "period");
    }
    rates_t r;
    stats_rates(&sum.acc, wear, &r);

    /* Rentabilité de la batterie (§9.9). */
    static roi_ctx_t rc;
    memset(&rc, 0, sizeof(rc));
    rc.wear = wear;
    int month = (int)(today_date / 100 % 100);
    rc.cur_ym = year * 12 + month - 1;
    history_foreach_month(roi_month, &rc);
    rates_t rt;
    stats_rates(&today, wear, &rt);
    rc.cum_net += rt.gain_batt_net;
    if (month >= 1 && month <= 12) {
        rc.last12[month - 1].observed = true;
        rc.last12[month - 1].gain_batt_net += rt.gain_batt_net;
        rc.last12[month - 1].days_observed += 1;
    }
    tariff_t tf;
    config_tariff(&s_cfg, &tf);
    roi_params_t rp = {
        .kwc = s_cfg.pv.kwc,
        .yield_kwh_kwc = s_cfg.roi.yield_kwh_kwc,
        .capa_kwh = s_cfg.batt.capa_kwh,
        .eta = s_cfg.batt.eta,
        .price_hp = tariff_mean_price(&tf, false),
        .price_sell = tf.price_sell,
        .ratio_surplus = (rc.total_days >= RATIO_SURPLUS_DAYS && rc.pv_kwh > 0.0f)
                         ? rc.surplus_kwh / rc.pv_kwh : RATIO_SURPLUS_DEF,
    };
    memcpy(rp.month_frac, s_cfg.roi.month_frac, sizeof(rp.month_frac));
    for (int i = 0; i < 12; i++) {
        rp.hist_surplus_day[i] = rc.old_days[i] > 0 ? rc.old_surplus[i] / (float)rc.old_days[i] : -1.0f;
    }
    roi_t roi;
    stats_roi(rc.last12, &rp, s_cfg.batt.price_eur, year, &roi);

    jw_begin(&s_jw, req);
    jw_printf(&s_jw, "{\"period\":\"%s\",\"days\":%d,", period, sum.days);
    emit_acc(&s_jw, &sum.acc);
    jw_printf(&s_jw, ",\"autoconso\":");
    jw_num(&s_jw, r.autoconso, 4);
    jw_printf(&s_jw, ",\"autoprod\":");
    jw_num(&s_jw, r.autoprod, 4);
    jw_printf(&s_jw, ",\"cost\":{\"real\":");
    jw_num(&s_jw, sum.acc.c_real, 3);
    jw_printf(&s_jw, ",\"no_batt\":");
    jw_num(&s_jw, sum.acc.c_no_batt, 3);
    jw_printf(&s_jw, ",\"no_install\":");
    jw_num(&s_jw, sum.acc.c_no_install, 3);
    jw_printf(&s_jw, "},\"gain\":{\"total\":");
    jw_num(&s_jw, r.gain_total, 3);
    jw_printf(&s_jw, ",\"batt\":");
    jw_num(&s_jw, r.gain_batt, 3);
    jw_printf(&s_jw, ",\"batt_net\":");
    jw_num(&s_jw, r.gain_batt_net, 3);
    jw_printf(&s_jw, ",\"ce\":");
    jw_num(&s_jw, r.gain_ce, 3);
    jw_printf(&s_jw, "},\"coverage\":");
    jw_num(&s_jw, sum.acc.secs_grid > 0.0f ? sum.acc.secs_full / sum.acc.secs_grid : NAN, 3);

    jw_printf(&s_jw, ",\"roi\":{\"price\":");
    jw_num(&s_jw, s_cfg.batt.price_eur, 2);
    jw_printf(&s_jw, ",\"wear_kwh\":");
    jw_num(&s_jw, wear, 4);
    jw_printf(&s_jw, ",\"cum_net\":");
    jw_num(&s_jw, rc.cum_net, 2);
    jw_printf(&s_jw, ",\"annual\":");
    jw_num(&s_jw, roi.annual_gain, 2);
    jw_printf(&s_jw, ",\"payback_years\":");
    jw_num(&s_jw, roi.payback_years, 2);
    jw_printf(&s_jw, ",\"spread\":%.2f,\"estimate\":%s,\"months_observed\":%d,\"ratio_surplus\":",
              (double)(roi.estimate ? ROI_SPREAD : 0.0f), roi.estimate ? "true" : "false", roi.months_observed);
    jw_num(&s_jw, rp.ratio_surplus, 3);
    uint32_t c = s_cfg.batt.commissioning;
    if (c != 0) {
        jw_printf(&s_jw, ",\"commissioning\":\"%04lu-%02lu-%02lu\"", (unsigned long)(c / 10000),
                  (unsigned long)(c / 100 % 100), (unsigned long)(c % 100));
    } else {
        jw_printf(&s_jw, ",\"commissioning\":null");
    }
    jw_printf(&s_jw, "}}");
    return jw_end(&s_jw);
}

/* ---------- POST /api/models/reset ---------- */

static esp_err_t models_reset_post(httpd_req_t *req)
{
    if (!web_check_admin(req)) {
        return web_send_error(req, WEB_401, "mot de passe administrateur invalide", NULL);
    }
    int n = web_read_body(req, s_body, sizeof(s_body));
    if (n < 0) {
        return web_send_error(req, WEB_400, "corps invalide", NULL);
    }
    cJSON *root = cJSON_ParseWithLength(s_body, (size_t)n);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "model");
    const char *which = cJSON_IsString(m) ? m->valuestring : "";
    bool all = strcmp(which, "all") == 0;
    bool pv = all || strcmp(which, "pv") == 0;
    bool base = all || strcmp(which, "base") == 0;
    bool prof = all || strcmp(which, "profile") == 0;
    cJSON_Delete(root);
    if (!pv && !base && !prof) {
        return web_send_error(req, WEB_400, "model attendu : pv, base, profile ou all", "model");
    }
    history_reset_models(pv, base, prof);
    cJSON *r = cJSON_CreateObject();
    if (r != NULL) {
        cJSON_AddBoolToObject(r, "ok", true);
    }
    return web_send_json(req, r);
}

esp_err_t api_hist_register(httpd_handle_t srv)
{
    const httpd_uri_t routes[] = {
        { .uri = "/api/history",      .method = HTTP_GET,  .handler = history_get },
        { .uri = "/api/forecast",     .method = HTTP_GET,  .handler = forecast_get_handler },
        { .uri = "/api/stats",        .method = HTTP_GET,  .handler = stats_get },
        { .uri = "/api/models/reset", .method = HTTP_POST, .handler = models_reset_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(srv, &routes[i]), TAG, "%s", routes[i].uri);
    }
    return ESP_OK;
}
