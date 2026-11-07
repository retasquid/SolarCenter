#pragma once

/* Plan journalier (CLAUDE.md §9.1, §9.3). C pur. Créneaux de 30 min à partir de t0. */

#include <stdbool.h>
#include <stdint.h>

#define PLAN_SLOTS      48
#define PLAN_SLOT_S     1800

typedef enum {
    CE_HC_ONLY = 0,         /* S < B : tout le solaire va à la batterie */
    CE_SOLAR_SURPLUS,       /* B ≤ S < B + E_ce : batterie d'abord, chauffe-eau sur le reste */
    CE_SOLAR_EARLY,         /* S ≥ B + E_ce : chauffe-eau autour du pic, la batterie absorbe les écarts */
} ce_mode_t;

typedef struct {
    int64_t t0;                     /* début du premier créneau */
    int     n;                      /* nombre de créneaux renseignés (≤ PLAN_SLOTS) */
    float   pv_wh[PLAN_SLOTS];      /* production prévue */
    float   base_wh[PLAN_SLOTS];    /* consommation de base prévue */
    bool    hc[PLAN_SLOTS];         /* créneau en heures creuses */
    float   soc;                    /* % actuel */
    float   soc_min;                /* % */
    float   soc_max;                /* % */
    float   capa_kwh;               /* capacité utile (entre soc_min et soc_max) */
    float   eta;
    float   wear_eur_kwh;
    float   price_hp;               /* prix des heures pleines à venir (base : prix base) */
    float   price_hc;
    float   price_sell;
    float   e_ce_wh;                /* énergie encore nécessaire au ballon (§9.5) */
    int64_t shower_at;              /* prochaine heure des douches */
    float   price[PLAN_SLOTS];      /* prix d'achat au milieu du créneau (€/kWh), pour l'arbitrage */
} plan_in_t;

typedef struct {
    float     S_wh;                 /* surplus solaire prévu jusqu'au prochain début d'HC */
    float     S_shower_wh;          /* surplus des créneaux qui se terminent avant les douches */
    float     B_wh;                 /* énergie à mettre en batterie pour les HP sans soleil */
    float     E_ce_wh;
    ce_mode_t mode;
    float     v_batt;               /* valeur d'un kWh stocké : prix_HP × η − usure (€/kWh) */
    float     v_ce;                 /* valeur d'un kWh chauffe-eau : prix_HC */
    float     v_sell;               /* valeur d'un kWh injecté : prix de revente */
    bool      ce_first;             /* le chauffe-eau passe avant la batterie */
    int64_t   t0;
    int       n;
    float     soc_start;                /* % au calcul du plan */
    float     soc_target[PLAN_SLOTS];   /* % en fin de créneau */
} plan_t;

void planner_compute(const plan_in_t *in, plan_t *out);

/* ---------- Arbitrage réseau (§9.6) ----------
 * Charger en HC n'a d'intérêt que si le kWh restitué en HP vaut plus que son achat en HC, ses pertes
 * et l'usure : prix_HP − prix_HC / η − usure > 0, et si le soleil du lendemain ne remplira pas la
 * batterie de toute façon. On simule la plage HP suivante (déficits servis, surplus solaire stocké,
 * batterie pleine) et on ne garde que la charge qui augmente le gain, prise dans les créneaux HC
 * les moins chers (le prix HC change à minuit avec un tarif semaine / week-end). */
#define ARB_STEP_WH     50.0f       /* pas de recherche de la quantité à charger */

typedef struct {
    int   hc_start;                 /* prochaine plage HC du plan [hc_start, hc_end[, -1 si aucune */
    int   hc_end;
    float gain_kwh;                 /* € par kWh restitué avec le créneau HC le moins cher, NAN si non évaluable */
    float charge_wh[PLAN_SLOTS];    /* énergie à prendre au réseau par créneau (Wh, côté AC) */
    float grid_wh;                  /* Σ charge_wh */
    float stored_wh;                /* énergie restituable ajoutée (= grid_wh × η) */
    float gain_eur;                 /* gain net attendu de la charge */
    float soc_target;               /* % à atteindre à la fin de la plage HC */
} arb_t;

void planner_arbitrage(const plan_in_t *in, float p_charge_max, float p_discharge_max, arb_t *out);

/* SOC cible à l'instant t (créneau en cours), soc_min hors du plan. */
float planner_soc_target(const plan_t *p, int64_t t, float soc_min);

const char *planner_mode_name(ce_mode_t m);
