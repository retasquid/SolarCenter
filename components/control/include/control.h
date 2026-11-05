#pragma once

/* Planification et régulation (CLAUDE.md §6, §9.3–§9.5, §10).
 * task_regul : plan, plan de nuit, regulator_step() toutes les 5 s, journal des décisions.
 * task_act   : envoi des commandes (Zendure, Tongou), seulement hors mode observateur. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "planner.h"
#include "regulator.h"

#define CONTROL_LOG_LEN 200

typedef struct {
    bool          running;          /* au moins un pas de régulation (heure NTP connue) */
    bool          observer;
    bool          plan_valid;
    plan_t        plan;
    wh_night_t    night;
    reg_output_t  out;              /* dernière décision */
    float         soc_target;
    int64_t       last_shower;
    int64_t       next_shower;
    float         e_full_wh;
    float         e_heated_wh;
    bool          tank_full;
    int64_t       last_full;
    uint8_t       override;         /* reg_override_t */
    int64_t       override_until;
    int64_t       updated;
} control_status_t;

/* Ligne du journal (binaire, mise en forme à la lecture). */
typedef struct {
    uint32_t ts;
    int16_t  p_grid;
    int16_t  p_batt;
    int16_t  batt_sp;
    int16_t  avail;
    uint8_t  soc;                   /* 255 = inconnu */
    uint8_t  flags;                 /* CTL_F_* */
    uint8_t  ce_reason;
    uint8_t  batt_reason;
    uint8_t  limits;
    uint8_t  mode;
} ctl_log_t;

#define CTL_F_RELAY     0x01
#define CTL_F_OBSERVER  0x02
#define CTL_F_HC        0x04
#define CTL_F_FULL      0x08
#define CTL_F_FALLBACK  0x10

esp_err_t control_start(void);

void control_status(control_status_t *out);

/* Copie des n dernières lignes, de la plus récente à la plus ancienne. Retourne le nombre copié. */
int control_log_read(ctl_log_t *out, int max);

/* Forçage utilisateur du chauffe-eau pour `minutes` (§9.4). */
esp_err_t control_override(reg_override_t mode, uint16_t minutes);
