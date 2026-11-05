#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* task_act (act.c). */
esp_err_t control_act_start(void);

/* Dernière décision du régulateur, envoyée par task_act selon ses garde-fous. */
void control_act_submit(float batt_sp, bool relay);
