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
                     float price_buy, float price_sell, float price_hc)
{
    if (!(dt_s > 0.0f && dt_s <= STATS_MAX_DT_S)) {
        return false;
    }
    flows_t f0, f1;
    flows_split(prev, &f0);
    flows_split(cur, &f1);

    float e_imp = trap(pos(prev->p_grid), pos(cur->p_grid), dt_s);
    float e_exp = trap(pos(-prev->p_grid), pos(-cur->p_grid), dt_s);
    /* Sans batterie : ce qu'elle a fourni aurait été importé, ce qu'elle a stocké exporté.
     * Séparation import/export après ce recalcul (§9.9). */
    float sb0 = prev->p_grid + prev->p_batt;
    float sb1 = cur->p_grid + cur->p_batt;
    float e_imp_sb = trap(pos(sb0), pos(sb1), dt_s);
    float e_exp_sb = trap(pos(-sb0), pos(-sb1), dt_s);
    float e_home = trap(f0.p_home, f1.p_home, dt_s);
    float e_ce_local = trap(f0.ce_local, f1.ce_local, dt_s);

    acc->e_pv += trap(pos(prev->p_pv), pos(cur->p_pv), dt_s);
    acc->e_import += e_imp;
    acc->e_export += e_exp;
    acc->e_home += e_home;
    acc->e_batt_ch += trap(pos(-prev->p_batt), pos(-cur->p_batt), dt_s);
    acc->e_batt_dis += trap(pos(prev->p_batt), pos(cur->p_batt), dt_s);
    acc->e_ce += trap(pos(prev->p_ce), pos(cur->p_ce), dt_s);
    acc->e_ce_local += e_ce_local;

    const float kwh = 1.0f / 1000.0f;
    acc->c_real += (e_imp * price_buy - e_exp * price_sell) * kwh;
    acc->c_no_batt += (e_imp_sb * price_buy - e_exp_sb * price_sell) * kwh;
    acc->c_no_install += e_home * price_buy * kwh;
    acc->hc_ce_value += e_ce_local * price_hc * kwh;
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
