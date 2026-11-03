#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "cfg_types.h"

/* Shelly Pro 3EM, capteur maître (CLAUDE.md §8.1). */
typedef struct {
    float total_w;      /* > 0 import, < 0 export */
    float phase_w[3];   /* a, b, c */
} shelly_reading_t;

esp_err_t drv_shelly_poll(const cfg_t *cfg, shelly_reading_t *out);

/* Analyse d'une réponse EM.GetStatus (séparée pour les tests sur réponses réelles). */
esp_err_t drv_shelly_parse(const char *json, size_t len, shelly_reading_t *out);
