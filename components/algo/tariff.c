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

/* Dimanche de Pâques (algorithme grégorien anonyme, dit de Meeus/Jones/Butcher). */
static void easter(int y, int *month, int *day)
{
    int a = y % 19, b = y / 100, c = y % 100, d = b / 4, e = b % 4;
    int f = (b + 8) / 25, g = (b - f + 1) / 3;
    int h = (19 * a + b - d - g + 15) % 30;
    int i = c / 4, k = c % 4;
    int l = (32 + 2 * e + 2 * i - h - k) % 7;
    int m = (a + 11 * h + 22 * l) / 451;
    *month = (h + l - 7 * m + 114) / 31;
    *day = ((h + l - 7 * m + 114) % 31) + 1;
}

static int day_of_year(int y, int m, int d)
{
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return cum[m - 1] + d + (leap && m > 2 ? 1 : 0);
}

bool tariff_is_holiday(int year, int month, int mday)
{
    static const uint8_t fixed[][2] = {
        { 1, 1 }, { 5, 1 }, { 5, 8 }, { 7, 14 }, { 8, 15 }, { 11, 1 }, { 11, 11 }, { 12, 25 },
    };
    for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (fixed[i][0] == month && fixed[i][1] == mday) {
            return true;
        }
    }
    int em, ed;
    easter(year, &em, &ed);
    int delta = day_of_year(year, month, mday) - day_of_year(year, em, ed);
    /* Lundi de Pâques, Ascension, lundi de Pentecôte. */
    return delta == 1 || delta == 39 || delta == 50;
}

bool tariff_is_weekend(const tariff_t *t, const tariff_day_t *d)
{
    if (d->wday == 0 || d->wday == 6) {
        return true;
    }
    return t->holidays_weekend && tariff_is_holiday(d->year, d->month, d->mday);
}

bool tariff_high_season(const tariff_t *t, int month)
{
    return month >= 1 && month <= 12 && (t->high_months & (1u << (month - 1))) != 0;
}

float tariff_price_at(const tariff_t *t, const tariff_day_t *d, uint16_t minute)
{
    if (!t->hphc) {
        return t->price_base;
    }
    bool hc = tariff_is_hc(t, minute);
    if (!t->grid) {
        return hc ? t->price_hc : t->price_hp;
    }
    int season = tariff_high_season(t, d->month) ? TARIFF_HIGH_SEASON : TARIFF_LOW_SEASON;
    int day = tariff_is_weekend(t, d) ? TARIFF_WEEKEND : TARIFF_WEEKDAY;
    return t->grid_price[season][day][hc ? TARIFF_HC : TARIFF_HP];
}

float tariff_mean_price(const tariff_t *t, bool hc)
{
    if (!t->hphc) {
        return t->price_base;
    }
    if (!t->grid) {
        return hc ? t->price_hc : t->price_hp;
    }
    /* Pondération : 5 jours de semaine sur 7, mois de saison haute sur 12. */
    int n_high = 0;
    for (int m = 1; m <= 12; m++) {
        n_high += tariff_high_season(t, m) ? 1 : 0;
    }
    float w_high = (float)n_high / 12.0f;
    int k = hc ? TARIFF_HC : TARIFF_HP;
    float sum = 0.0f;
    for (int s = 0; s < 2; s++) {
        float ws = (s == TARIFF_HIGH_SEASON) ? w_high : 1.0f - w_high;
        sum += ws * (5.0f / 7.0f * t->grid_price[s][TARIFF_WEEKDAY][k] +
                     2.0f / 7.0f * t->grid_price[s][TARIFF_WEEKEND][k]);
    }
    return sum;
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
