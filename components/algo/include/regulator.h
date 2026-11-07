#pragma once

/* Décision temps réel (CLAUDE.md §9.4, §9.5, §10). Fonction pure : état → commandes, toutes les 5 s.
 * C pur ; le temps (epoch, s) est passé en entrée. */

#include <stdbool.h>
#include <stdint.h>
#include "planner.h"
#include "waterheater.h"

#define REG_DEADBAND_W      30.0f
#define REG_RAMP_W          300.0f      /* variation max de consigne par pas */
#define REG_CE_START_S      120         /* surplus suffisant depuis 2 min */
#define REG_CE_STOP_S       180         /* énergie insuffisante depuis 3 min */
#define REG_THERMO_W        100.0f      /* relais ON et p_ce sous ce seuil … */
#define REG_THERMO_S        180         /* … pendant 3 min : coupure du thermostat */

typedef enum {
    REG_OVR_AUTO = 0,
    REG_OVR_ON,
    REG_OVR_OFF,
} reg_override_t;

/* Raisons de la consigne batterie. */
typedef enum {
    RB_NO_TIME = 0,         /* pas d'heure NTP : repos */
    RB_FALLBACK,            /* Shelly invalide : repos */
    RB_NO_BATT,             /* Zendure invalide : batterie ignorée (consigne 0) */
    RB_DEADBAND,            /* |p_grid| < 30 W : consigne inchangée */
    RB_TRACK,               /* asservissement zéro injection / zéro soutirage */
    RB_ARBITRAGE,           /* charge depuis le réseau en HC, rentable selon le plan (§9.6) */
} reg_batt_reason_t;

/* Limites appliquées à la consigne batterie (bits). */
#define RB_LIM_SOC_MIN      0x01    /* décharge interdite : SOC ≤ SOC min */
#define RB_LIM_SOC_MAX      0x02    /* charge interdite : SOC ≥ SOC max */
#define RB_LIM_HC           0x04    /* pas de décharge en heures creuses */
#define RB_LIM_CE           0x08    /* sous la cible du plan, la batterie n'alimente pas le chauffe-eau */
#define RB_LIM_RAMP         0x10
#define RB_LIM_POWER        0x20

/* Raisons de la commande du chauffe-eau. */
typedef enum {
    RC_NO_TIME = 0,
    RC_FALLBACK,            /* Shelly invalide : OFF hors HC */
    RC_OVERRIDE_ON,
    RC_OVERRIDE_OFF,
    RC_TANK_FULL,
    RC_CATCHUP,             /* rattrapage avant les douches */
    RC_NIGHT,               /* plan de nuit en HC */
    RC_SOLAR_WAIT,          /* surplus suffisant, attente des 2 min */
    RC_SOLAR_START,
    RC_SOLAR_RUN,
    RC_SOLAR_STOP,          /* énergie insuffisante depuis 3 min */
    RC_HC_ONLY,             /* mode du plan : rien en journée */
    RC_IDLE,                /* pas assez de surplus */
    RC_MIN_ON,              /* maintenu ON : durée min */
    RC_MIN_OFF,             /* maintenu OFF : durée min */
    RC_COUNT
} reg_ce_reason_t;

typedef struct {
    float    p_charge_max;
    float    p_discharge_max;
    float    soc_min;
    float    soc_max;
    float    solar_on_w;        /* seuil de démarrage solaire */
    uint32_t min_on_s;
    uint32_t min_off_s;
    wh_cfg_t wh;
} reg_cfg_t;

typedef struct {
    const reg_cfg_t *cfg;
    int64_t   now;
    float     dt_s;             /* depuis le pas précédent */
    bool      time_ok;          /* heure NTP valide */
    bool      hc;               /* heures creuses en ce moment */
    bool      grid_ok;          /* Shelly valide (sinon mode repli) */
    float     p_grid;
    bool      batt_ok;          /* Zendure valide */
    float     p_batt;
    float     soc;
    bool      ce_ok;            /* Tongou valide */
    float     p_ce;
    bool      relay_on;         /* état mesuré du relais */
    ce_mode_t mode;             /* du plan */
    float     soc_target;       /* % du plan pour ce créneau */
    wh_night_t night;           /* plan de nuit en vigueur */
    int64_t   last_shower;      /* dernière heure des douches passée */
    int64_t   next_shower;      /* prochaine heure des douches */
    reg_override_t override;
    int64_t   override_until;
    float     arb_charge_w;     /* charge réseau demandée pour ce créneau HC (W, ≥ 0), 0 = pas d'arbitrage */
    float     arb_soc_target;   /* % où s'arrêter */
} reg_input_t;

typedef struct {
    float      batt_sp;         /* dernière consigne (W, > 0 décharge) */
    bool       relay_cmd;       /* dernière commande du relais */
    int64_t    relay_changed_at;
    bool       solar_run;       /* relais allumé par le routage solaire */
    int64_t    avail_ok_since;  /* 0 = condition non remplie */
    int64_t    avail_low_since;
    int64_t    thermo_low_since;
    bool       catchup_active;  /* rattrapage lancé : maintenu jusqu'à la coupure du thermostat */
    int64_t    night_seen;      /* début du plan de nuit suivi */
    float      e_night_wh;      /* énergie fournie pendant ce plan de nuit */
    wh_state_t wh;
} reg_state_t;

typedef struct {
    float   batt_sp;
    uint8_t batt_reason;        /* reg_batt_reason_t */
    uint8_t batt_limits;        /* RB_LIM_* */
    bool    relay;
    uint8_t ce_reason;          /* reg_ce_reason_t */
    float   avail_w;            /* surplus disponible pour le chauffe-eau */
    int64_t catchup_at;
    bool    tank_full_event;
    bool    new_cycle_event;
    bool    catchup_blocked;    /* rattrapage dû mais interdit en HP : « eau peut-être tiède » */
} reg_output_t;

/* Démarrage (§10) : batterie au repos, relais OFF, durée min OFF considérée comme écoulée. */
void regulator_init(reg_state_t *s, const reg_cfg_t *c, int64_t now);

void regulator_step(const reg_input_t *in, reg_state_t *s, reg_output_t *out);

const char *regulator_ce_reason_name(uint8_t r);
const char *regulator_batt_reason_name(uint8_t r);
