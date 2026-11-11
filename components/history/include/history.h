#pragma once

/* Historique (CLAUDE.md §6, §9.2, §9.9) : énergies intégrées à chaque mesure du Shelly,
 * créneaux de 30 min, agrégats journaliers et mensuels dans LittleFS, apprentissage des modèles. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_state.h"
#include "stats.h"
#include "rls.h"

#define HIST_SLOT_S         1800
#define HIST_SLOTS_KEPT     336     /* 7 jours */
#define HIST_DAYS_KEPT      366

typedef struct {
    uint32_t ts;                /* début du créneau, epoch */
    float    e_pv;              /* Wh */
    float    e_import;
    float    e_export;
    float    e_home;
    float    e_batt_ch;
    float    e_batt_dis;
    float    e_ce;
    float    pv_pred;           /* Wh prévus pour ce créneau, NAN sans prévision */
    float    base_pred;
    uint8_t  soc;               /* % en fin de créneau, 255 = inconnu */
    uint8_t  cov_grid;          /* % du créneau mesuré par le Shelly */
    uint8_t  cov_full;          /* % du créneau avec toutes les mesures */
    uint8_t  flags;             /* HIST_SLOT_* (octet réservé, à 0, dans les fichiers antérieurs) */
} hist_slot_t;

#define HIST_SLOT_REBUILT   0x01    /* PV de l'Envoy reconstitué après une coupure (compteur cumulé) */
#define HIST_SLOT_HOME_OK   0x02    /* … et maison reconstituée aussi (SolarFlow mesuré pendant la coupure) */

typedef struct {
    uint32_t     date;          /* AAAAMMJJ */
    energy_acc_t acc;
} hist_day_t;

typedef struct {
    uint32_t     month;         /* AAAAMM */
    uint16_t     days;
    uint16_t     reserved;
    energy_acc_t acc;
} hist_month_t;

/* Prévision pour un créneau. */
typedef struct {
    float gti;                  /* W/m² (NAN sans prévision) */
    float temp;                 /* °C */
    float pv_raw;               /* repli physique, Wh */
    float pv;                   /* modèle RLS (ou repli s'il n'est pas prêt), Wh */
    float base_prof;            /* profil EMA, Wh */
    float base;                 /* modèle RLS (ou profil), Wh */
} hist_pred_t;

/* Les callbacks retournent faux pour arrêter le parcours. */
typedef bool (*hist_slot_cb_t)(const hist_slot_t *s, void *ctx);
typedef bool (*hist_day_cb_t)(const hist_day_t *d, void *ctx);
typedef bool (*hist_month_cb_t)(const hist_month_t *m, void *ctx);

/* task_hist. LittleFS doit être monté sur /fs. */
esp_err_t history_start(void);

/* Appelé par task_poll après chaque lecture du Shelly. */
void history_on_sample(const app_state_t *st);

/* Énergies d'aujourd'hui (créneaux clos + en cours). Faux si l'heure n'est pas connue. */
bool history_today(energy_acc_t *out, uint32_t *date);

/* Créneaux de [from_ts, to_ts], du plus ancien au plus récent. */
esp_err_t history_foreach_slot(uint32_t from_ts, uint32_t to_ts, hist_slot_cb_t cb, void *ctx);

/* Les n_days jours précédant aujourd'hui, du plus ancien au plus récent. */
esp_err_t history_foreach_day(int n_days, hist_day_cb_t cb, void *ctx);

/* Tous les mois enregistrés, du plus ancien au plus récent (jours clos uniquement). */
esp_err_t history_foreach_month(hist_month_cb_t cb, void *ctx);

bool history_predict(uint32_t slot_ts, hist_pred_t *out);

/* Copie des modèles ; profile_slots = créneaux du profil déjà mesurés (sur 96). */
void history_model_info(rls_t *pv, rls_t *base, int *profile_slots);

esp_err_t history_reset_models(bool pv, bool base, bool profile);

/* Coupures de l'Envoy : état de la reconstitution (page Système). */
typedef struct {
    bool     active;            /* coupure en cours, mesures du trou accumulées */
    uint32_t active_since;      /* epoch du début de la coupure en cours */
    uint32_t last_ts;           /* epoch de la dernière reconstitution, 0 = aucune */
    float    last_pv_wh;        /* énergie PV reconstituée */
    float    last_hours;        /* durée de la coupure reconstituée */
    uint16_t count;             /* reconstitutions depuis le démarrage */
    uint16_t dropped;           /* coupures non reconstituées (compteur absent ou incohérent, trop longues) */
} hist_gap_info_t;

void history_gap_info(hist_gap_info_t *out);

/* Rendement aller-retour de la batterie : appris (stats.h, eta_learn) après ETA_READY_SAMPLES cycles,
 * sinon fallback (valeur des Paramètres). *info peut être NULL. */
float history_batt_eta(float fallback, eta_learn_t *info);

/* Outils de date locale. */
uint32_t history_date_of(int64_t t);                /* AAAAMMJJ */
int32_t  history_daynum(uint32_t date);             /* jours depuis le 1970-01-01 */
uint32_t history_date_from_daynum(int32_t daynum);
