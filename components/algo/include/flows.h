#pragma once

/* Répartition des flux d'énergie (CLAUDE.md §9.9), partagée par le diagramme et les statistiques.
 * C pur : uniquement <stdint.h>, <stdbool.h>, <math.h>, <string.h>. */

#include <stdbool.h>

typedef struct {
    float p_pv;     /* ≥ 0 */
    float p_grid;   /* > 0 import, < 0 export */
    float p_batt;   /* > 0 décharge, < 0 charge */
    float p_ce;     /* ≥ 0 */
} flows_in_t;

/* Toutes les valeurs sont ≥ 0, en W. */
typedef struct {
    float p_home;       /* p_grid + p_pv + p_batt, borné à ≥ 0 */
    float p_base;       /* p_home − p_ce, borné à ≥ 0 */
    float pv_home;
    float pv_batt;
    float pv_grid;
    float batt_home;
    float batt_grid;    /* décharge non absorbée par la maison (ne devrait pas arriver en régulation) */
    float grid_home;
    float grid_batt;
    float home_ce;      /* = p_ce */
    float ce_local;     /* part du chauffe-eau couverte par le PV et la batterie */
} flows_t;

void flows_split(const flows_in_t *in, flows_t *out);
