#include <math.h>
#include "sanity.h"

#define GRID_MAX_W  15000.0f
#define PV_MARGIN   1.2f
#define BATT_MAX_W  3000.0f
#define CE_MAX_W    4000.0f

bool sanity_ok(qty_t q, float v, float kwc)
{
    if (!isfinite(v)) {
        return false;
    }
    switch (q) {
    case QTY_GRID:
        return fabsf(v) < GRID_MAX_W;
    case QTY_PV:
        return v >= 0.0f && v < PV_MARGIN * kwc * 1000.0f;
    case QTY_BATT:
        return fabsf(v) < BATT_MAX_W;
    case QTY_CE:
        return v >= 0.0f && v < CE_MAX_W;
    case QTY_SOC:
        return v >= 0.0f && v <= 100.0f;
    }
    return false;
}
