#include <math.h>
#include <string.h>
#include "waterheater.h"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void wh_init(wh_state_t *s, const wh_cfg_t *c)
{
    memset(s, 0, sizeof(*s));
    s->e_full_wh = c->power_w * c->full_heat_min / 60.0f;
}

float wh_seconds_for(const wh_cfg_t *c, float e_wh)
{
    if (!(c->power_w > 0.0f) || !(e_wh > 0.0f)) {
        return 0.0f;
    }
    return e_wh / c->power_w * 3600.0f;
}

float wh_remaining(const wh_state_t *s)
{
    if (s->tank_full) {
        return 0.0f;
    }
    /* Ballon pas encore plein alors que E_full a été fourni : E_full est sous-estimée (grosse
     * consommation, pertes). On garde un reste minimal pour que le rattrapage parte assez tôt. */
    float floor = WH_MIN_REMAINING_FRAC * s->e_full_wh;
    return clampf(s->e_full_wh - s->e_heated_wh, floor, s->e_full_wh);
}

void wh_new_cycle(wh_state_t *s, int64_t now)
{
    s->tank_full = false;
    s->e_heated_wh = 0.0f;
    s->cycle_start = now;
    s->run_from_cold = false;
}

void wh_on_tank_full(wh_state_t *s, int64_t now)
{
    if (s->run_from_cold && s->e_heated_wh > 0.0f && isfinite(s->e_heated_wh)) {
        s->e_full_wh = (1.0f - WH_EFULL_ALPHA) * s->e_full_wh + WH_EFULL_ALPHA * s->e_heated_wh;
    }
    s->tank_full = true;
    s->last_full = now;
    s->run_from_cold = false;
}

float wh_s_ce(const wh_cfg_t *c, float s_wh, float b_wh)
{
    float v = s_wh - b_wh;
    return v > 0.0f ? v * c->forecast_margin : 0.0f;
}

bool wh_night_plan(const wh_cfg_t *c, const wh_state_t *s, const wh_window_t *windows, int n,
                   int64_t shower_at, float s_ce_wh, wh_night_t *out)
{
    memset(out, 0, sizeof(*out));
    float remaining = wh_remaining(s);
    float e = clampf(remaining - s_ce_wh, 0.0f, s->e_full_wh);
    if (e < WH_NIGHT_MIN_WH) {
        return false;
    }

    /* Plage qui se termine le plus près avant les douches. */
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (windows[i].end > windows[i].start && windows[i].end <= shower_at &&
            (best < 0 || windows[i].end > windows[best].end)) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }

    const wh_window_t *w = &windows[best];
    int64_t dur = (int64_t)lroundf(wh_seconds_for(c, e));
    int64_t start = w->end - dur - (int64_t)c->start_margin_min * 60;
    if (start < w->start) {
        start = w->start;       /* plage trop courte : on démarre à son début */
    }
    out->active = true;
    out->start = start;
    out->end = w->end;
    out->e_target_wh = e;
    out->full = e >= remaining - 1.0f;
    return true;
}

int64_t wh_catchup_at(const wh_cfg_t *c, const wh_state_t *s, int64_t shower_at)
{
    int64_t dur = (int64_t)lroundf(wh_seconds_for(c, wh_remaining(s) * WH_CATCHUP_SAFETY));
    return shower_at - dur - (int64_t)c->start_margin_min * 60;
}
