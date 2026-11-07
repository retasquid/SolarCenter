#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cfg_types.h"

/* Enphase Envoy / IQ Gateway (CLAUDE.md §8.2). */
typedef struct {
    float w_now;        /* production instantanée, W */
    bool  from_eim;     /* compteur de production (eim) ; faux = repli sur les micro-onduleurs */
    bool     wh_valid;      /* compteur cumulé lu */
    uint32_t wh_lifetime;   /* production cumulée de la même source, Wh (reconstitution des coupures) */
} envoy_reading_t;

esp_err_t drv_envoy_poll(const cfg_t *cfg, envoy_reading_t *out);

esp_err_t drv_envoy_parse(const char *json, size_t len, envoy_reading_t *out);
