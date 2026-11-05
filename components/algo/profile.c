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
    if (!index_ok(day_type, slot)) {
        return PROFILE_DEFAULT_WH;
    }
    if (p->n[day_type][slot] > 0) {
        return p->wh[day_type][slot];
    }
    /* Créneau jamais mesuré : même créneau de l'autre type de jour… */
    int other = 1 - day_type;
    if (p->n[other][slot] > 0) {
        return p->wh[other][slot];
    }
    /* … sinon moyenne des créneaux appris de ce type de jour, puis de tous ; 300 Wh si le profil est vide. */
    for (int pass = 0; pass < 2; pass++) {
        float sum = 0.0f;
        int cnt = 0;
        for (int d = 0; d < PROFILE_DAY_TYPES; d++) {
            if (pass == 0 && d != day_type) {
                continue;
            }
            for (int s = 0; s < PROFILE_SLOTS; s++) {
                if (p->n[d][s] > 0) {
                    sum += p->wh[d][s];
                    cnt++;
                }
            }
        }
        if (cnt > 0) {
            return sum / (float)cnt;
        }
    }
    return PROFILE_DEFAULT_WH;
}

int profile_learned_slots(const profile_t *p)
{
    int cnt = 0;
    for (int d = 0; d < PROFILE_DAY_TYPES; d++) {
        for (int s = 0; s < PROFILE_SLOTS; s++) {
            cnt += p->n[d][s] > 0;
        }
    }
    return cnt;
}
