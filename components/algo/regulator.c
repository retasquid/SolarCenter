#include <math.h>
#include <string.h>
#include "regulator.h"

static float pos(float v)
{
    return v > 0.0f ? v : 0.0f;
}

void regulator_init(reg_state_t *s, const reg_cfg_t *c, int64_t now)
{
    memset(s, 0, sizeof(*s));
    s->relay_changed_at = now - (int64_t)c->min_off_s;
    wh_init(&s->wh, &c->wh);
    s->wh.cycle_start = now;
}

/* ---------- Chauffe-eau ---------- */

static void track_energy(const reg_input_t *in, reg_state_t *s)
{
    if (in->night.active && in->night.start != s->night_seen) {
        s->night_seen = in->night.start;       /* nouveau plan de nuit */
        s->e_night_wh = 0.0f;
    }
    if (!in->ce_ok || !(in->p_ce > 0.0f) || !(in->dt_s > 0.0f)) {
        return;
    }
    float e = in->p_ce * in->dt_s / 3600.0f;
    s->wh.e_heated_wh += e;
    if (in->night.active && in->now >= in->night.start && in->now < in->night.end) {
        s->e_night_wh += e;
    }
}

/* Relais ON et puissance quasi nulle pendant 3 min : le thermostat a coupé, ballon plein. */
static bool detect_tank_full(const reg_input_t *in, reg_state_t *s)
{
    if (s->relay_cmd && in->relay_on && in->ce_ok && in->p_ce < REG_THERMO_W) {
        if (s->thermo_low_since == 0) {
            s->thermo_low_since = in->now;
        } else if (!s->wh.tank_full && in->now - s->thermo_low_since >= REG_THERMO_S) {
            wh_on_tank_full(&s->wh, in->now);
            return true;
        }
    } else {
        s->thermo_low_since = 0;
    }
    return false;
}

/* Routage solaire (§9.4). */
static bool solar_decision(const reg_input_t *in, reg_state_t *s, float avail_start, float avail_keep,
                           uint8_t *reason)
{
    const reg_cfg_t *c = in->cfg;
    if (s->relay_cmd && s->solar_run) {
        if (avail_keep < c->wh.power_w) {
            if (s->avail_low_since == 0) {
                s->avail_low_since = in->now;
            }
            if (in->now - s->avail_low_since >= REG_CE_STOP_S) {
                *reason = RC_SOLAR_STOP;
                return false;
            }
        } else {
            s->avail_low_since = 0;
        }
        *reason = RC_SOLAR_RUN;
        return true;
    }
    s->avail_low_since = 0;
    bool ok = avail_start >= c->solar_on_w && (!in->batt_ok || in->soc >= in->soc_target);
    if (!ok) {
        s->avail_ok_since = 0;
        *reason = RC_IDLE;
        return false;
    }
    if (s->avail_ok_since == 0) {
        s->avail_ok_since = in->now;
    }
    if (in->now - s->avail_ok_since >= REG_CE_START_S) {
        *reason = RC_SOLAR_START;
        return true;
    }
    *reason = RC_SOLAR_WAIT;
    return false;
}

static void decide_ce(const reg_input_t *in, reg_state_t *s, reg_output_t *out)
{
    const reg_cfg_t *c = in->cfg;
    float ce = in->ce_ok ? in->p_ce : 0.0f;
    float charge = in->batt_ok ? pos(-in->p_batt) : 0.0f;
    bool batt_allowed = in->batt_ok && in->soc > in->soc_target;
    float avail_start = -in->p_grid + ce + charge;
    float avail_keep = -in->p_grid + ce + (in->batt_ok ? -in->p_batt : 0.0f) +
                       (batt_allowed ? c->p_discharge_max : 0.0f);
    out->avail_w = avail_start;

    bool in_night = in->night.active && in->now >= in->night.start && in->now < in->night.end;
    bool night_need = in_night && (in->night.full || s->e_night_wh < in->night.e_target_wh);
    out->catchup_at = wh_catchup_at(&c->wh, &s->wh, in->next_shower);
    /* Une fois lancé, le rattrapage continue jusqu'à la coupure du thermostat (§9.5) : sinon l'heure
     * recalculée sur le reste, qui diminue, le ferait s'arrêter et repartir. */
    if (s->wh.tank_full) {
        s->catchup_active = false;
    }
    bool catchup_due = s->catchup_active || (in->now >= out->catchup_at && in->now < in->next_shower);
    bool override = in->override != REG_OVR_AUTO && in->now < in->override_until;

    bool want = false;
    bool safety = false;
    uint8_t reason = RC_IDLE;
    if (!in->time_ok) {
        safety = true;
        reason = RC_NO_TIME;
    } else if (!in->grid_ok) {
        /* Repli : OFF hors HC ; en HC, seules les chauffes prévues continuent. */
        safety = true;
        reason = RC_FALLBACK;
        want = in->hc && !s->wh.tank_full && (night_need || catchup_due);
    } else if (override) {
        want = in->override == REG_OVR_ON;
        reason = want ? RC_OVERRIDE_ON : RC_OVERRIDE_OFF;
    } else if (s->wh.tank_full) {
        reason = RC_TANK_FULL;
    } else if (catchup_due && (c->wh.catchup_hp || in->hc)) {
        want = true;
        reason = RC_CATCHUP;
        s->catchup_active = true;
    } else if (night_need) {
        want = true;
        reason = RC_NIGHT;
    } else {
        out->catchup_blocked = catchup_due;
        if (in->mode == CE_HC_ONLY) {
            reason = RC_HC_ONLY;
        } else {
            want = solar_decision(in, s, avail_start, avail_keep, &reason);
        }
    }
    bool solar_branch = reason == RC_SOLAR_WAIT || reason == RC_SOLAR_START || reason == RC_SOLAR_RUN ||
                        reason == RC_SOLAR_STOP || reason == RC_IDLE;
    if (!solar_branch) {
        s->avail_ok_since = 0;      /* les 2 min de surplus se comptent en routage solaire seulement */
    }

    /* Durées min ON/OFF, sauf sécurité (repli, heure inconnue). */
    if (want != s->relay_cmd && !safety) {
        int64_t elapsed = in->now - s->relay_changed_at;
        if (s->relay_cmd && elapsed < (int64_t)c->min_on_s) {
            want = true;
            reason = RC_MIN_ON;
        } else if (!s->relay_cmd && elapsed < (int64_t)c->min_off_s) {
            want = false;
            reason = RC_MIN_OFF;
        }
    }

    if (want != s->relay_cmd) {
        s->relay_cmd = want;
        s->relay_changed_at = in->now;
        s->thermo_low_since = 0;
        /* E_full n'est appris que sur une chauffe partie d'un ballon froid, menée jusqu'à la coupure. */
        s->wh.run_from_cold = want && s->wh.e_heated_wh < 1.0f;
    }
    if (!want || reason == RC_CATCHUP || reason == RC_NIGHT || reason == RC_OVERRIDE_ON) {
        s->solar_run = false;
    } else if (reason == RC_SOLAR_START) {
        s->solar_run = true;
    }
    out->relay = s->relay_cmd;
    out->ce_reason = reason;
}

/* ---------- Batterie ---------- */

static void decide_batt(const reg_input_t *in, reg_state_t *s, reg_output_t *out)
{
    const reg_cfg_t *c = in->cfg;
    if (!in->time_ok || !in->grid_ok || !in->batt_ok) {
        s->batt_sp = 0.0f;
        out->batt_sp = 0.0f;
        out->batt_reason = !in->time_ok ? RB_NO_TIME : (!in->grid_ok ? RB_FALLBACK : RB_NO_BATT);
        return;
    }
    float prev = s->batt_sp;
    float target;
    if (fabsf(in->p_grid) < REG_DEADBAND_W) {
        target = prev;
        out->batt_reason = RB_DEADBAND;
    } else {
        target = in->p_batt + in->p_grid;   /* vise 0 W au compteur */
        out->batt_reason = RB_TRACK;
    }

    float max_dis = c->p_discharge_max;
    float max_ch = c->p_charge_max;
    uint8_t lim = 0;
    if (in->soc <= c->soc_min) {
        max_dis = 0.0f;
        lim |= RB_LIM_SOC_MIN;
    }
    if (in->soc >= c->soc_max) {
        max_ch = 0.0f;
        lim |= RB_LIM_SOC_MAX;
    }
    if (in->hc) {
        max_dis = 0.0f;     /* §9.4 : jamais de décharge en HC (arbitrage hors M3) */
        lim |= RB_LIM_HC;
    }
    /* Sous la cible du plan, le chauffe-eau ne puise pas dans la batterie (§9.3) : elle ne couvre
     * que la consommation hors chauffe-eau, quelle que soit la raison de la chauffe. */
    if (s->relay_cmd && in->ce_ok && in->soc <= in->soc_target) {
        float without_ce = in->p_batt + in->p_grid - in->p_ce;
        if (target > without_ce) {
            target = without_ce;
            lim |= RB_LIM_CE;
        }
    }

    float sp = target;
    if (sp > prev + REG_RAMP_W) {
        sp = prev + REG_RAMP_W;
        lim |= RB_LIM_RAMP;
    } else if (sp < prev - REG_RAMP_W) {
        sp = prev - REG_RAMP_W;
        lim |= RB_LIM_RAMP;
    }
    if (sp > max_dis) {
        sp = max_dis;
        lim |= RB_LIM_POWER;
    } else if (sp < -max_ch) {
        sp = -max_ch;
        lim |= RB_LIM_POWER;
    }
    if (!isfinite(sp)) {
        sp = 0.0f;
    }
    s->batt_sp = sp;
    out->batt_sp = sp;
    out->batt_limits = lim;
}

void regulator_step(const reg_input_t *in, reg_state_t *s, reg_output_t *out)
{
    memset(out, 0, sizeof(*out));
    if (in->time_ok && in->last_shower > s->wh.cycle_start) {
        wh_new_cycle(&s->wh, in->last_shower);
        s->catchup_active = false;
        out->new_cycle_event = true;
    }
    track_energy(in, s);
    out->tank_full_event = detect_tank_full(in, s);
    decide_ce(in, s, out);
    decide_batt(in, s, out);
}

const char *regulator_ce_reason_name(uint8_t r)
{
    static const char *const names[RC_COUNT] = {
        "pas d'heure", "repli Shelly", "forçage ON", "forçage OFF", "ballon plein", "rattrapage",
        "plan de nuit", "attente surplus", "démarrage solaire", "solaire", "arrêt solaire",
        "HC uniquement", "pas de surplus", "durée min ON", "durée min OFF",
    };
    return r < RC_COUNT ? names[r] : "?";
}

const char *regulator_batt_reason_name(uint8_t r)
{
    static const char *const names[] = {
        "pas d'heure", "repli Shelly", "batterie ignorée", "zone morte", "zéro injection",
    };
    return r <= RB_TRACK ? names[r] : "?";
}
