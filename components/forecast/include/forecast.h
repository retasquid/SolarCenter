#pragma once

/* Client Open-Meteo (CLAUDE.md §8.5) : seul appel internet du projet.
 * ⚠ API gratuite réservée à un usage non commercial. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define FORECAST_HOURS 48       /* aujourd'hui et demain, depuis minuit local */

typedef struct {
    bool    valid;
    int64_t t0;                         /* epoch de la première heure */
    int64_t fetched;                    /* epoch de la dernière réception */
    uint8_t n;
    float   gti[FORECAST_HOURS];        /* W/m², moyenne de l'heure précédente */
    float   temp[FORECAST_HOURS];       /* °C, instantanée */
} forecast_t;

/* task_forecast : toutes les heures, plus 05:30. */
esp_err_t forecast_start(void);

void forecast_get(forecast_t *out);

/* Irradiance et température interpolées à l'instant t (milieu d'un créneau). Faux si indisponible. */
bool forecast_at(int64_t t, float *gti, float *temp);

/* Demande une nouvelle lecture au prochain réveil de la tâche (coordonnées modifiées…). */
void forecast_refresh(void);

esp_err_t forecast_parse(const char *json, size_t len, forecast_t *out);
