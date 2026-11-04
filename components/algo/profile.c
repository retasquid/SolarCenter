#include <math.h>
#include <string.h>
#include "profile.h"

void profile_init(profile_t *p)
{
    memset(p, 0, sizeof(*p));
}

int profile_day_type(int weekday)
{
    return (weekday == 0 || weekday == 6) ? 1 : 0;
}

static bool index_ok(int day_type, int slot)
{
    return day_type >= 0 && day_type < PROFILE_DAY_TYPES && slot >= 0 && slot < PROFILE_SLOTS;
}

bool profile_update(profile_t *p, int day_type, int slot, float wh)
{
    if (!index_ok(day_type, slot) || !isfinite(wh) || wh < 0.0f) {
        return false;
    }
    float *v = &p->wh[day_type][slot];
    uint16_t *n = &p->n[day_type][slot];
    *v = (*n == 0) ? wh : (1.0f - PROFILE_ALPHA) * *v + PROFILE_ALPHA * wh;
    if (*n < UINT16_MAX) {
        (*n)++;
    }
    return true;
}

float profile_get(const profile_t *p, int day_type, int slot)
{
    if (!index_ok(day_type, slot) || p->n[day_type][slot] == 0) {
        return PROFILE_DEFAULT_WH;
    }
    return p->wh[day_type][slot];
}
