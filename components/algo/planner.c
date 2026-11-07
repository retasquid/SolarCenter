#include <math.h>
#include <string.h>
#include "planner.h"

static float pos(float v)
{
    return v > 0.0f && isfinite(v) ? v : 0.0f;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void planner_compute(const plan_in_t *in, plan_t *out)
{
    memset(out, 0, sizeof(*out));
    int n = in->n < 0 ? 0 : (in->n > PLAN_SLOTS ? PLAN_SLOTS : in->n);
    out->t0 = in->t0;
    out->n = n;
    out->soc_start = in->soc;

    /* Fin de la « journée » : prochain début d'HC après le premier créneau HP. */
    int h0 = 0;
    while (h0 < n && in->hc[h0]) {
        h0++;
    }
    int end = h0;
    while (end < n && !in->hc[end]) {
        end++;
    }

    int first_sun = -1;
    for (int k = 0; k < end; k++) {
        float s = pos(in->pv_wh[k] - in->base_wh[k]);
        out->S_wh += s;
        if (in->t0 + (int64_t)(k + 1) * PLAN_SLOT_S <= in->shower_at) {
            out->S_shower_wh += s;
        }
        if (first_sun < 0 && s > 0.0f) {
            first_sun = k;
        }
    }

    /* B : déficit des créneaux HP sans soleil, à partir du premier créneau en surplus (la batterie ne
     * peut pas être chargée avant), jusqu'au début d'HC ; plafonné à la place libre (§9.3). */
    float capa_wh = in->capa_kwh * 1000.0f;
    float span = in->soc_max - in->soc_min;
    float frac_now = span > 0.0f ? clampf((in->soc - in->soc_min) / span, 0.0f, 1.0f) : 0.0f;
    float e_now = frac_now * capa_wh;
    float deficit = 0.0f;
    for (int k = (first_sun < 0 ? end : first_sun); k < end; k++) {
        if (!in->hc[k]) {
            deficit += pos(in->base_wh[k] - in->pv_wh[k]);
        }
    }
    float room = capa_wh - e_now;
    out->B_wh = deficit < room ? deficit : (room > 0.0f ? room : 0.0f);
    out->E_ce_wh = pos(in->e_ce_wh);

    /* Mode du chauffe-eau. Sans aucun surplus prévu, rien à router. */
    if (out->S_wh <= 0.0f || out->S_wh < out->B_wh) {
        out->mode = CE_HC_ONLY;
    } else if (out->S_wh < out->B_wh + out->E_ce_wh) {
        out->mode = CE_SOLAR_SURPLUS;
    } else {
        out->mode = CE_SOLAR_EARLY;
    }

    /* Valeur de chaque kWh de surplus (§9.1) : l'ordre découle des prix, il n'est pas codé en dur. */
    out->v_batt = in->price_hp * in->eta - in->wear_eur_kwh;
    out->v_ce = in->price_hc;
    out->v_sell = in->price_sell;
    out->ce_first = out->mode == CE_SOLAR_EARLY || out->v_ce > out->v_batt;

    /* SOC cible : B est apporté au fil du surplus prévu, et atteint à la fin de la production. */
    float cum = 0.0f;
    for (int k = 0; k < n; k++) {
        if (k < end) {
            cum += pos(in->pv_wh[k] - in->base_wh[k]);
        }
        float f = out->S_wh > 0.0f ? cum / out->S_wh : 0.0f;
        float e = e_now + out->B_wh * f;
        out->soc_target[k] = capa_wh > 0.0f ? in->soc_min + clampf(e / capa_wh, 0.0f, 1.0f) * span : in->soc_min;
    }
}

/* Batterie (énergie restituable, Wh) sur les créneaux [k0, k1[ hors HC : les déficits sont servis
 * (au plus p_dis par créneau), le surplus solaire est stocké avec les pertes et l'excédent est exporté.
 * Retourne la valeur (€) de l'énergie servie et exportée ; *e est mis à jour. */
static float simulate_hp(const plan_in_t *in, int k0, int k1, float capa, float p_dis_wh, float *e)
{
    float v = 0.0f;
    for (int k = k0; k < k1; k++) {
        if (in->hc[k]) {
            continue;       /* en HC la batterie ne se décharge pas (§9.4) */
        }
        float net = (isfinite(in->pv_wh[k]) ? in->pv_wh[k] : 0.0f) - (isfinite(in->base_wh[k]) ? in->base_wh[k] : 0.0f);
        if (net < 0.0f) {
            float d = -net;
            d = d < *e ? d : *e;
            d = d < p_dis_wh ? d : p_dis_wh;
            *e -= d;
            v += d * (in->price[k] - in->wear_eur_kwh) / 1000.0f;
        } else {
            *e += net * in->eta;
            if (*e > capa) {
                v += (*e - capa) / in->eta * in->price_sell / 1000.0f;
                *e = capa;
            }
        }
    }
    return v;
}

/* Coût (€) de grid_wh pris dans les créneaux HC les moins chers ; NAN si la plage n'y suffit pas.
 * Si charge != NULL, y inscrit la répartition. */
static float charge_cost(const plan_in_t *in, const int *order, int n_order, float slot_wh, float grid_wh,
                         float *charge)
{
    float left = grid_wh, cost = 0.0f;
    for (int i = 0; i < n_order && left > 0.0f; i++) {
        float take = left < slot_wh ? left : slot_wh;
        cost += take * in->price[order[i]] / 1000.0f;
        if (charge != NULL) {
            charge[order[i]] = take;
        }
        left -= take;
    }
    return left > 0.5f ? NAN : cost;
}

void planner_arbitrage(const plan_in_t *in, float p_charge_max, float p_discharge_max, arb_t *out)
{
    memset(out, 0, sizeof(*out));
    out->hc_start = out->hc_end = -1;
    out->gain_kwh = NAN;
    out->soc_target = in->soc;
    int n = in->n < 0 ? 0 : (in->n > PLAN_SLOTS ? PLAN_SLOTS : in->n);
    int k0 = 0;
    while (k0 < n && !in->hc[k0]) {
        k0++;
    }
    int k1 = k0;
    while (k1 < n && in->hc[k1]) {
        k1++;
    }
    int k2 = k1;
    while (k2 < n && !in->hc[k2]) {
        k2++;
    }
    if (k0 >= n || k1 >= n || !(in->eta > 0.0f)) {
        return;     /* pas de plage HC suivie d'heures pleines dans l'horizon */
    }
    out->hc_start = k0;
    out->hc_end = k1;

    /* Créneaux HC par prix croissant (tri par sélection, n ≤ 48). */
    int order[PLAN_SLOTS];
    int n_order = 0;
    for (int k = k0; k < k1; k++) {
        order[n_order++] = k;
    }
    for (int i = 0; i < n_order; i++) {
        for (int j = i + 1; j < n_order; j++) {
            if (in->price[order[j]] < in->price[order[i]]) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    float hp_min = INFINITY;
    for (int k = k1; k < k2; k++) {
        hp_min = in->price[k] < hp_min ? in->price[k] : hp_min;
    }
    out->gain_kwh = hp_min - in->price[order[0]] / in->eta - in->wear_eur_kwh;
    if (!(out->gain_kwh > 0.0f) || !(p_charge_max > 0.0f)) {
        return;
    }
    /* Seuls les créneaux rentables par eux-mêmes sont utilisés (le prix HC change à minuit). */
    while (n_order > 0 && !(hp_min - in->price[order[n_order - 1]] / in->eta - in->wear_eur_kwh > 0.0f)) {
        n_order--;
    }

    float capa = in->capa_kwh * 1000.0f;
    float span = in->soc_max - in->soc_min;
    float frac = span > 0.0f ? clampf((in->soc - in->soc_min) / span, 0.0f, 1.0f) : 0.0f;
    float e0 = frac * capa;
    float p_dis_wh = p_discharge_max * PLAN_SLOT_S / 3600.0f;
    (void)simulate_hp(in, 0, k0, capa, p_dis_wh, &e0);     /* heures pleines avant la plage */
    float e_ref = e0;
    float v_ref = simulate_hp(in, k1, k2, capa, p_dis_wh, &e_ref);

    float slot_wh = p_charge_max * PLAN_SLOT_S / 3600.0f;
    float best = 0.0f, best_x = 0.0f;
    for (float x = ARB_STEP_WH; e0 + x <= capa + 0.5f; x += ARB_STEP_WH) {
        float cost = charge_cost(in, order, n_order, slot_wh, x / in->eta, NULL);
        if (!isfinite(cost)) {
            break;      /* plus de place dans la plage HC */
        }
        float e = e0 + x;
        float gain = simulate_hp(in, k1, k2, capa, p_dis_wh, &e) - v_ref - cost;
        if (gain > best + 1e-4f) {
            best = gain;
            best_x = x;
        }
    }
    if (best_x <= 0.0f) {
        return;
    }
    out->grid_wh = best_x / in->eta;
    (void)charge_cost(in, order, n_order, slot_wh, out->grid_wh, out->charge_wh);
    out->stored_wh = best_x;
    out->gain_eur = best;
    out->soc_target = capa > 0.0f ? in->soc_min + clampf((e0 + best_x) / capa, 0.0f, 1.0f) * span : in->soc;
}

float planner_soc_target(const plan_t *p, int64_t t, float soc_min)
{
    if (p->n <= 0 || t < p->t0) {
        return soc_min;
    }
    int64_t k = (t - p->t0) / PLAN_SLOT_S;
    if (k >= p->n) {
        return soc_min;
    }
    /* Cible de fin du créneau précédent : celle qu'on doit avoir atteinte maintenant. */
    return k == 0 ? p->soc_start : p->soc_target[k - 1];
}

const char *planner_mode_name(ce_mode_t m)
{
    switch (m) {
    case CE_HC_ONLY:        return "hc_only";
    case CE_SOLAR_SURPLUS:  return "solar_surplus";
    case CE_SOLAR_EARLY:    return "solar_early";
    }
    return "?";
}
