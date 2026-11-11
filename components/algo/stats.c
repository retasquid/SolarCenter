#include <math.h>
#include <string.h>
#include "stats.h"

static float pos(float v)
{
    return v > 0.0f ? v : 0.0f;
}

/* Énergie d'un pas (Wh) entre deux puissances (W), méthode des trapèzes. */
static float trap(float a, float b, float dt_s)
{
    return (a + b) * 0.5f * dt_s / 3600.0f;
}

bool stats_integrate(energy_acc_t *acc, const flows_in_t *prev, const flows_in_t *cur, float dt_s,
                     bool full, float price_buy, float price_sell, float price_hc)
{
    if (!(dt_s > 0.0f && dt_s <= STATS_MAX_DT_S)) {
        return false;
    }
    const float kwh = 1.0f / 1000.0f;
    float e_imp = trap(pos(prev->p_grid), pos(cur->p_grid), dt_s);
    float e_exp = trap(pos(-prev->p_grid), pos(-cur->p_grid), dt_s);
    acc->e_import += e_imp;
    acc->e_export += e_exp;
    float c_step = (e_imp * price_buy - e_exp * price_sell) * kwh;
    acc->c_real += c_step;
    acc->secs_grid += dt_s;
    if (!full) {
        /* Contrefactuels inconnus : gain neutre sur ce pas. */
        acc->c_no_batt += c_step;
        acc->c_no_install += c_step;
        return true;
    }

    flows_t f0, f1;
    flows_split(prev, &f0);
    flows_split(cur, &f1);
    /* Sans batterie : ce qu'elle a fourni aurait été importé, ce qu'elle a stocké exporté.
     * Séparation import/export après ce recalcul (§9.9). */
    float sb0 = prev->p_grid + prev->p_batt;
    float sb1 = cur->p_grid + cur->p_batt;
    float e_imp_sb = trap(pos(sb0), pos(sb1), dt_s);
    float e_exp_sb = trap(pos(-sb0), pos(-sb1), dt_s);
    float e_home = trap(f0.p_home, f1.p_home, dt_s);
    float e_ce_local = trap(f0.ce_local, f1.ce_local, dt_s);

    acc->e_pv += trap(pos(prev->p_pv), pos(cur->p_pv), dt_s);
    acc->e_home += e_home;
    acc->e_batt_ch += trap(pos(-prev->p_batt), pos(-cur->p_batt), dt_s);
    acc->e_batt_dis += trap(pos(prev->p_batt), pos(cur->p_batt), dt_s);
    acc->e_ce += trap(pos(prev->p_ce), pos(cur->p_ce), dt_s);
    acc->e_ce_local += e_ce_local;
    acc->e_pv_home += trap(f0.pv_home, f1.pv_home, dt_s);
    acc->e_pv_batt += trap(f0.pv_batt, f1.pv_batt, dt_s);
    acc->e_pv_grid += trap(f0.pv_grid, f1.pv_grid, dt_s);
    acc->e_grid_home += trap(f0.grid_home, f1.grid_home, dt_s);
    acc->e_grid_batt += trap(f0.grid_batt, f1.grid_batt, dt_s);
    acc->e_batt_home += trap(f0.batt_home, f1.batt_home, dt_s);

    acc->c_no_batt += (e_imp_sb * price_buy - e_exp_sb * price_sell) * kwh;
    acc->c_no_install += e_home * price_buy * kwh;
    acc->hc_ce_value += e_ce_local * price_hc * kwh;
    acc->secs_full += dt_s;
    return true;
}

void stats_add(energy_acc_t *dst, const energy_acc_t *src)
{
    float *d = (float *)dst;
    const float *s = (const float *)src;
    for (unsigned i = 0; i < sizeof(*dst) / sizeof(float); i++) {
        d[i] += s[i];
    }
}

void stats_gap_integrate(gap_acc_t *g, const flows_in_t *prev, const flows_in_t *cur, float dt_s,
                         bool batt_ok, float price_buy, float price_hc)
{
    if (!(dt_s > 0.0f && dt_s <= STATS_MAX_DT_S)) {
        return;
    }
    float e_imp = trap(pos(prev->p_grid), pos(cur->p_grid), dt_s);
    g->e_import += e_imp;
    g->e_export += trap(pos(-prev->p_grid), pos(-cur->p_grid), dt_s);
    g->e_batt_ch += trap(pos(-prev->p_batt), pos(-cur->p_batt), dt_s);
    g->e_batt_dis += trap(pos(prev->p_batt), pos(cur->p_batt), dt_s);
    g->e_ce += trap(pos(prev->p_ce), pos(cur->p_ce), dt_s);
    g->buy_eur += e_imp * price_buy / 1000.0f;
    g->price_secs += price_buy * dt_s;
    g->price_hc_secs += price_hc * dt_s;
    g->secs += dt_s;
    if (!batt_ok) {
        g->batt_bad = 1;
    }
}

static float minf(float a, float b)
{
    return a < b ? a : b;
}

void stats_gap_fill(const gap_acc_t *g, float e_pv, float price_sell, energy_acc_t *d)
{
    memset(d, 0, sizeof(*d));
    float pv = pos(e_pv);
    d->e_pv = pv;
    if (g->batt_bad || !(g->secs > 0.0f)) {
        return;     /* PV seul : sans la batterie, la maison n'est pas déductible */
    }
    const float kwh = 1.0f / 1000.0f;
    float ch = g->e_batt_ch, dis = g->e_batt_dis;

    /* Totaux de la période, même répartition que flows_split() (§9.9) : l'export vient du PV. */
    float pv_grid = minf(g->e_export, pv);
    float pv_batt = minf(pv - pv_grid, ch);
    float pv_home = pv - pv_grid - pv_batt;
    float grid_batt = ch - pv_batt;
    float grid_home = pos(g->e_import - grid_batt);
    float batt_home = dis;
    float home = pv_home + batt_home + grid_home;
    float ce = minf(g->e_ce, home);
    float ce_local = home > 0.0f ? ce * (pv_home + batt_home) / home : 0.0f;

    d->e_home = home;
    d->e_batt_ch = ch;
    d->e_batt_dis = dis;
    d->e_ce = g->e_ce;
    d->e_ce_local = ce_local;
    d->e_pv_home = pv_home;
    d->e_pv_batt = pv_batt;
    d->e_pv_grid = pv_grid;
    d->e_grid_home = grid_home;
    d->e_grid_batt = grid_batt;
    d->e_batt_home = batt_home;

    /* Contrefactuels au prix moyen de la période ; le coût réel exact est déjà compté. */
    float price = g->price_secs / g->secs;
    float price_hc = g->price_hc_secs / g->secs;
    float c_real = g->buy_eur - g->e_export * price_sell * kwh;
    float pv_home_sb = minf(pv, home);      /* sans batterie : le PV va d'abord à la maison */
    float c_no_batt = ((home - pv_home_sb) * price - (pv - pv_home_sb) * price_sell) * kwh;
    d->c_no_batt = c_no_batt - c_real;
    d->c_no_install = home * price * kwh - c_real;
    d->hc_ce_value = ce_local * price_hc * kwh;
}

void eta_learn_init(eta_learn_t *e)
{
    memset(e, 0, sizeof(*e));
}

static void eta_restart(eta_learn_t *e, int soc, int64_t now)
{
    e->active = true;
    e->soc_ref = (uint8_t)soc;
    e->left_ref = false;
    e->t_start = now;
    e->e_ch = 0.0f;
    e->e_dis = 0.0f;
}

bool eta_learn_step(eta_learn_t *e, bool ok, float p0, float p1, float dt_s, int soc, int64_t now)
{
    if (!ok || soc < 0 || soc > 100 || !(dt_s > 0.0f && dt_s <= STATS_MAX_DT_S) || !isfinite(p0) || !isfinite(p1)) {
        e->active = false;      /* mesure manquante : le bilan du cycle ne serait plus juste */
        return false;
    }
    if (!e->active || now - e->t_start > ETA_MAX_CYCLE_S) {
        eta_restart(e, soc, now);
        return false;
    }
    e->e_ch += trap(pos(-p0), pos(-p1), dt_s);
    e->e_dis += trap(pos(p0), pos(p1), dt_s);
    if (soc != e->soc_ref) {
        e->left_ref = true;
        return false;
    }
    if (!e->left_ref || e->e_ch < ETA_MIN_CH_WH || e->e_dis < ETA_MIN_DIS_WH) {
        return false;           /* retour au SOC de départ, mais trop peu d'énergie : on continue */
    }
    float eta = e->e_dis / e->e_ch;
    bool good = eta >= ETA_LO && eta <= ETA_HI;
    if (good) {
        e->last = eta;
        e->eta = e->samples == 0 ? eta : (1.0f - ETA_ALPHA) * e->eta + ETA_ALPHA * eta;
        if (e->samples < UINT16_MAX) {
            e->samples++;
        }
    }
    eta_restart(e, soc, now);   /* le cycle suivant part d'ici */
    return good;
}

float eta_learn_value(const eta_learn_t *e, float fallback)
{
    return e->samples >= ETA_READY_SAMPLES && e->eta >= ETA_LO && e->eta <= ETA_HI ? e->eta : fallback;
}

float stats_wear_cost(float batt_price, float cycles, float capa_kwh)
{
    if (!(batt_price > 0.0f && cycles > 0.0f && capa_kwh > 0.0f)) {
        return 0.0f;
    }
    return batt_price / (cycles * capa_kwh);
}

void stats_rates(const energy_acc_t *acc, float wear_cost, rates_t *out)
{
    out->autoconso = acc->e_pv > 0.0f ? (acc->e_pv - acc->e_export) / acc->e_pv : NAN;
    if (out->autoconso < 0.0f) {
        out->autoconso = 0.0f;
    }
    out->autoprod = acc->e_home > 0.0f ? 1.0f - acc->e_import / acc->e_home : NAN;
    if (out->autoprod < 0.0f) {
        out->autoprod = 0.0f;
    }
    out->gain_batt = acc->c_no_batt - acc->c_real;
    out->gain_batt_net = out->gain_batt - acc->e_batt_dis / 1000.0f * wear_cost;
    out->gain_total = acc->c_no_install - acc->c_real;
    out->gain_ce = acc->hc_ce_value;
}

int stats_days_in_month(int year, int month)
{
    static const uint8_t d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) {
        return 30;
    }
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return d[month - 1] + (month == 2 && leap ? 1 : 0);
}

void stats_roi(const roi_month_t months[12], const roi_params_t *p, float batt_price, int year, roi_t *out)
{
    memset(out, 0, sizeof(*out));
    float spread = p->eta * p->price_hp - p->price_sell;
    float total = 0.0f;
    for (int m = 0; m < 12; m++) {
        int days = stats_days_in_month(year, m + 1);
        if (months[m].observed && months[m].days_observed > 0) {
            total += months[m].gain_batt_net * (float)days / (float)months[m].days_observed;
            out->months_observed++;
            continue;
        }
        float surplus_day = p->hist_surplus_day[m];
        if (!(surplus_day >= 0.0f)) {
            surplus_day = p->kwc * p->yield_kwh_kwc * p->month_frac[m] / 100.0f * p->ratio_surplus / (float)days;
        }
        float cycled = surplus_day < p->capa_kwh ? surplus_day : p->capa_kwh;
        total += (float)days * cycled * spread;
    }
    out->annual_gain = total;
    out->payback_years = total > 0.0f ? batt_price / total : NAN;
    out->estimate = out->months_observed < 12;
}
