#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cfg_types.h"

/* Zendure SolarFlow 1600 AC+, API locale zenSDK (CLAUDE.md §8.3). Lecture seule au M1. */
typedef struct {
    char  sn[CFG_SN_LEN];
    float soc;                  /* % */
    float output_home_w;
    float grid_input_w;
    float pack_input_w;
    float output_pack_w;
    float p_batt;               /* côté AC : > 0 décharge, < 0 charge */
    int   ac_mode;
    float input_limit_w;
    float output_limit_w;
    int   soc_set_pm;           /* ‰ */
    int   min_soc_pm;           /* ‰ */
} zendure_reading_t;

esp_err_t drv_zendure_poll(const cfg_t *cfg, zendure_reading_t *out);

esp_err_t drv_zendure_parse(const char *json, size_t len, zendure_reading_t *out);
