#pragma once

/* Stratégie du chauffe-eau (CLAUDE.md §9.5). C pur ; temps en secondes epoch passé en paramètre. */

#include <stdbool.h>
#include <stdint.h>

#define WH_NIGHT_MIN_WH     300.0f      /* en dessous : pas de chauffe de nuit, le soleil s'en charge */
#define WH_EFULL_ALPHA      0.2f        /* moyenne glissante de E_full */
#define WH_MIN_REMAINING_FRAC 0.2f      /* reste minimal tant que le thermostat n'a pas coupé */
/* E_full est une moyenne : un soir de forte consommation demande davantage. La garantie (§9.5) prime :
 * le rattrapage part plus tôt, le thermostat coupe de toute façon quand le ballon est plein. */
#define WH_CATCHUP_SAFETY   1.5f

typedef struct {
    float    power_w;
    float    full_heat_min;     /* durée de chauffe complète initiale */
    float    forecast_margin;   /* marge sur le surplus solaire prévu */
    uint16_t start_margin_min;
    bool     catchup_hp;        /* rattrapage autorisé en heures pleines */
} wh_cfg_t;

typedef struct {
    float   e_full_wh;          /* énergie d'une chauffe complète, apprise */
    float   e_heated_wh;        /* énergie chauffée depuis le début du cycle */
    bool    tank_full;          /* coupure du thermostat constatée dans ce cycle */
    int64_t cycle_start;        /* heure des douches qui a ouvert le cycle */
    int64_t last_full;          /* dernière coupure du thermostat, 0 = jamais */
    bool    run_from_cold;      /* chauffe en cours démarrée ballon froid (apprentissage de E_full) */
} wh_state_t;

typedef struct {
    int64_t start;              /* plage heures creuses [start, end[ */
    int64_t end;
} wh_window_t;

typedef struct {
    bool    active;
    int64_t start;              /* début de la chauffe de nuit */
    int64_t end;                /* fin de la plage HC choisie */
    float   e_target_wh;        /* énergie à fournir */
    bool    full;               /* chauffe complète : jusqu'à la coupure du thermostat */
} wh_night_t;

void  wh_init(wh_state_t *s, const wh_cfg_t *c);

/* Durée (s) pour fournir e_wh à la puissance nominale. */
float wh_seconds_for(const wh_cfg_t *c, float e_wh);

/* Énergie encore nécessaire pour remplir le ballon dans ce cycle (0 si tank_full). */
float wh_remaining(const wh_state_t *s);

/* Nouveau cycle à l'heure des douches : tank_full = faux, énergie chauffée = 0. */
void  wh_new_cycle(wh_state_t *s, int64_t now);

/* Coupure du thermostat : apprend E_full si la chauffe est partie d'un ballon froid. */
void  wh_on_tank_full(wh_state_t *s, int64_t now);

/* S_ce = max(0, S − B) × marge (§9.5). */
float wh_s_ce(const wh_cfg_t *c, float s_wh, float b_wh);

/* Plan de nuit : plage HC qui se termine le plus près avant les douches, chauffe placée au plus tard.
 * windows : plages HC du cycle. Retourne out->active. */
bool  wh_night_plan(const wh_cfg_t *c, const wh_state_t *s, const wh_window_t *windows, int n,
                    int64_t shower_at, float s_ce_wh, wh_night_t *out);

/* Instant du rattrapage : douches − T(reste) − marge. */
int64_t wh_catchup_at(const wh_cfg_t *c, const wh_state_t *s, int64_t shower_at);
