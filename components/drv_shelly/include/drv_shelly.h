#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cfg_types.h"

/* Shelly Pro 3EM, capteur maître (CLAUDE.md §8.1).
 * Profil triphasé : EM.GetStatus. Profil « monophase » : EM1.GetStatus sur le canal du réseau. */
typedef struct {
    float total_w;      /* > 0 import, < 0 export */
    float phase_w[3];   /* a, b, c (triphasé) ; en monophasé, seul [0] est renseigné */
    bool  mono;         /* lu par EM1 (profil monophasé) */
} shelly_reading_t;

esp_err_t drv_shelly_poll(const cfg_t *cfg, shelly_reading_t *out);

/* Analyses séparées (tests sur réponses réelles). */
esp_err_t drv_shelly_parse(const char *json, size_t len, shelly_reading_t *out);
esp_err_t drv_shelly_parse_em1(const char *json, size_t len, shelly_reading_t *out);
