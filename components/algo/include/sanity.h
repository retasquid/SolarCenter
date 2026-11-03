#pragma once

/* Bornes de plausibilité des mesures (CLAUDE.md §7). C pur. */

#include <stdbool.h>

typedef enum {
    QTY_GRID = 0,   /* |p_grid| < 15000 W */
    QTY_PV,         /* 0 ≤ p_pv < 1,2 × kWc × 1000 */
    QTY_BATT,       /* |p_batt| < 3000 W */
    QTY_CE,         /* 0 ≤ p_ce < 4000 W */
    QTY_SOC,        /* 0 ≤ soc ≤ 100 % */
} qty_t;

/* kwc n'est utilisé que pour QTY_PV. Faux pour NaN et ±inf. */
bool sanity_ok(qty_t q, float v, float kwc);
