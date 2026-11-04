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
