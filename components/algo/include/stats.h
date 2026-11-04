#pragma once

/* Compteurs d'énergie, coûts, taux et gains (CLAUDE.md §9.9). C pur. */

#include <stdbool.h>
#include <stdint.h>
#include "flows.h"

#define STATS_MAX_DT_S 10.0f    /* pas plus long ignoré (§9.9) */

/* Énergies en Wh, coûts en €. */
typedef struct {
    float e_pv;
    float e_import;
    float e_export;
    float e_home;
    float e_batt_ch;
    float e_batt_dis;
    float e_ce;
    float e_ce_local;
    float c_real;           /* import × prix(t) − export × prix_revente */
    float c_no_batt;        /* même calcul avec p_grid + p_batt */
    float c_no_install;     /* e_home × prix(t) */
    float hc_ce_value;      /* e_ce_local × prix_HC, en € (routage du chauffe-eau) */
} energy_acc_t;

/* Intègre un pas par la méthode des trapèzes entre deux échantillons.
 * Retourne faux (rien n'est ajouté) si dt ≤ 0 ou dt > STATS_MAX_DT_S. */
bool stats_integrate(energy_acc_t *acc, const flows_in_t *prev, const flows_in_t *cur, float dt_s,
                     float price_buy, float price_sell, float price_hc);

void stats_add(energy_acc_t *dst, const energy_acc_t *src);

/* coût_usure = prix / (cycles × capacité utile), €/kWh ; 0 si non renseigné. */
float stats_wear_cost(float batt_price, float cycles, float capa_kwh);

typedef struct {
    float autoconso;        /* (E_pv − E_export) / E_pv, NAN si E_pv = 0 */
    float autoprod;         /* 1 − E_import / E_home, NAN si E_home = 0 */
    float gain_batt;        /* c_no_batt − c_real */
    float gain_batt_net;    /* gain_batt − E_batt_dis × coût_usure */
    float gain_total;       /* c_no_install − c_real */
    float gain_ce;          /* routage du chauffe-eau */
} rates_t;

void stats_rates(const energy_acc_t *acc, float wear_cost, rates_t *out);

/* ---------- Rentabilité de la batterie ---------- */

typedef struct {
    bool  observed;             /* au moins un jour mesuré ce mois-ci */
    float gain_batt_net;        /* €, mois observé */
    uint8_t days_observed;
} roi_month_t;

typedef struct {
    float kwc;
    float yield_kwh_kwc;        /* productible annuel */
    float month_frac[12];       /* % */
    float capa_kwh;
    float eta;
    float price_hp;
    float price_sell;
    float ratio_surplus;        /* surplus / PV observé, 0,5 par défaut */
    float hist_surplus_day[12]; /* kWh/j moyen observé les années précédentes, < 0 = inconnu */
} roi_params_t;

typedef struct {
    float annual_gain;          /* €/an projeté */
    float payback_years;        /* NAN si gain ≤ 0 */
    int   months_observed;
    bool  estimate;             /* moins de 12 mois observés : fourchette ±20 % */
} roi_t;

/* Mois non observé : jours × min(capa, surplus_jour) × (η·prix_HP − prix_revente) (§9.9).
 * Un mois partiellement observé est extrapolé à sa durée complète. */
void stats_roi(const roi_month_t months[12], const roi_params_t *p, float batt_price, int year, roi_t *out);

int stats_days_in_month(int year, int month);   /* month : 1–12 */
