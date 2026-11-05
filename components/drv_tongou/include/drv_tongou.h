#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cfg_types.h"

/* Interrupteur Tongou du chauffe-eau sous OpenBeken, API compatible Tasmota (CLAUDE.md §8.4).
 * Lecture seule au M1. */
typedef struct {
    bool  relay_on;
    float power_w;
} tongou_reading_t;

/* À appeler avant toute lecture ou commande (crée le verrou du pilote). */
esp_err_t drv_tongou_init(void);

esp_err_t drv_tongou_poll(const cfg_t *cfg, tongou_reading_t *out);

/* Relais du chauffe-eau (OpenBeken : POWER ON/OFF ; Tuya : DP du relais). */
esp_err_t drv_tongou_set(const cfg_t *cfg, bool on);

esp_err_t drv_tongou_parse_power(const char *json, size_t len, float *power_w);
esp_err_t drv_tongou_parse_relay(const char *json, size_t len, bool *on);
