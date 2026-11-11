#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* task_act (act.c). */
esp_err_t control_act_start(void);

/* Dernière décision du régulateur, envoyée par task_act selon ses garde-fous. */
void control_act_submit(float batt_sp, bool relay);

/* Faux tant que la dernière consigne écrite au SolarFlow n'a pas pu être appliquée puis remesurée :
 * délai minimal entre deux écritures pas écoulé, ou mesure du Shelly ou du SolarFlow antérieure à
 * l'écriture. Le régulateur garde alors sa consigne (pas de double correction). */
bool control_act_settled(uint32_t now_ms, uint32_t grid_ts_ms, uint32_t batt_ts_ms);
