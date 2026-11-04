#include <string.h>     /* NULL */
#include "tariff.h"

#define MINUTES_PER_DAY 1440

static bool in_slot(uint16_t start, uint16_t end, uint16_t m)
{
    if (start < end) {
        return m >= start && m < end;
    }
    /* Passe minuit (end = 0 : jusqu'à minuit). */
    return m >= start || m < end;
}

bool tariff_is_hc(const tariff_t *t, uint16_t minute)
{
    if (!t->hphc) {
        return false;
    }
    minute %= MINUTES_PER_DAY;
    for (int i = 0; i < t->n_hc && i < TARIFF_SLOTS_MAX; i++) {
        if (t->hc[i].start_min != t->hc[i].end_min &&
            in_slot(t->hc[i].start_min, t->hc[i].end_min, minute)) {
            return true;
        }
    }
    return false;
}

float tariff_price(const tariff_t *t, uint16_t minute)
{
    if (!t->hphc) {
        return t->price_base;
    }
    return tariff_is_hc(t, minute) ? t->price_hc : t->price_hp;
}

uint16_t tariff_next_change(const tariff_t *t, uint16_t minute, bool *next_is_hc)
{
    minute %= MINUTES_PER_DAY;
    bool cur = tariff_is_hc(t, minute);
    uint16_t best = 0;
    /* Les changements ne peuvent avoir lieu qu'à un début ou une fin de plage. */
    for (int i = 0; t->hphc && i < t->n_hc && i < TARIFF_SLOTS_MAX; i++) {
        uint16_t edges[2] = { t->hc[i].start_min, t->hc[i].end_min };
        for (int e = 0; e < 2; e++) {
            uint16_t d = (uint16_t)((edges[e] + MINUTES_PER_DAY - minute) % MINUTES_PER_DAY);
            if (d == 0) {
                d = MINUTES_PER_DAY;
            }
            if (tariff_is_hc(t, (uint16_t)((minute + d) % MINUTES_PER_DAY)) != cur && (best == 0 || d < best)) {
                best = d;
            }
        }
    }
    if (next_is_hc != NULL) {
        *next_is_hc = best != 0 ? !cur : cur;
    }
    return best;
}
