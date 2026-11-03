#include <string.h>
#include "flows.h"

static float pos(float v)
{
    return v > 0.0f ? v : 0.0f;
}

static float minf(float a, float b)
{
    return a < b ? a : b;
}

void flows_split(const flows_in_t *in, flows_t *out)
{
    memset(out, 0, sizeof(*out));

    float pv = pos(in->p_pv);
    float charge = pos(-in->p_batt);
    float dis = pos(in->p_batt);
    float home = pos(in->p_grid + pv + in->p_batt);

    out->p_home = home;
    out->p_base = pos(home - pos(in->p_ce));

    out->pv_home = minf(pv, home);
    out->pv_batt = pos(minf(pv - out->pv_home, charge));
    out->pv_grid = pos(pv - out->pv_home - out->pv_batt);
    out->batt_home = pos(minf(dis, home - out->pv_home));
    out->batt_grid = pos(dis - out->batt_home);
    out->grid_home = pos(home - out->pv_home - out->batt_home);
    out->grid_batt = pos(charge - out->pv_batt);

    out->home_ce = pos(in->p_ce);
    if (home > 0.0f) {
        out->ce_local = out->home_ce * (out->pv_home + out->batt_home) / home;
    }
}
