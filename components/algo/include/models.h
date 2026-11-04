#pragma once

/* Modèles de prévision par créneau de 30 min (CLAUDE.md §9.2). C pur.
 * Énergies en Wh par créneau, irradiance en W/m², températures en °C. */

#include <stdbool.h>
#include <stdint.h>
#include "rls.h"

#define MODELS_SLOT_S           1800
#define MODELS_PV_GTI_MIN       20.0f       /* échantillon PV appris seulement au-delà */
#define MODELS_PV_LAMBDA        0.998f
#define MODELS_BASE_LAMBDA      0.998f
#define MODELS_BASE_MAX_WH      7500.0f     /* 15 kW pendant 30 min (borne |p_grid|) */
#define MODELS_HEAT_REF_C       15.0f       /* terme de chauffage : max(0, 15 − T) */

/* Valeur à l'instant t à partir d'une série horaire h[0..n-1] commençant à t0.
 * Le point i représente l'instant t0 + i·3600 + center_offset_s :
 *   −1800 pour une moyenne de l'heure précédente (irradiance Open-Meteo), 0 pour une valeur instantanée.
 * Interpolation linéaire, bornée aux extrémités ; NAN hors de la série ou si un voisin est NAN. */
float models_hourly_at(const float *h, int n, int64_t t0, int32_t center_offset_s, int64_t t);

/* ---------- Production PV ---------- */

void  models_pv_init(rls_t *r, float kwc);
void  models_pv_x(float gti, float temp, float x[3]);   /* [gti, gti·(T − 25), 1] */
float models_pv_fallback(float kwc, float gti);         /* kWc × 0,85 × gti × 0,5 (Wh) */
float models_pv_max(float kwc);                         /* 1,2 × kWc × 1000 × 0,5 (Wh) */
/* Modèle si prêt (≥ 96 échantillons), sinon repli ; borné à [0, max]. */
float models_pv_predict(const rls_t *r, float kwc, float gti, float temp);

/* ---------- Consommation de base (hors chauffe-eau) ---------- */

void  models_base_init(rls_t *r);
void  models_base_x(float prof, float temp, float x[3]); /* [prof, max(0, 15 − T), 1] ; T inconnue → 0 */
float models_base_predict(const rls_t *r, float prof, float temp);
