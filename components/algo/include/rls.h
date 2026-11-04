#pragma once

/* Moindres carrés récursifs avec facteur d'oubli (CLAUDE.md §9.2). C pur, tout en float, n ≤ 4.
 *   k = P·x / (λ + xᵀ·P·x)
 *   e = y − θᵀ·x
 *   θ = θ + k·e
 *   P = (P − k·xᵀ·P) / λ */

#include <stdbool.h>
#include <stdint.h>

#define RLS_N_MAX           4
#define RLS_TRACE_MAX       1e6f
#define RLS_MIN_SAMPLES     96      /* 2 jours de créneaux de 30 min avant d'utiliser le modèle */

typedef struct {
    uint8_t  n;
    float    lambda;
    float    theta[RLS_N_MAX];
    float    P[RLS_N_MAX][RLS_N_MAX];
    float    theta0[RLS_N_MAX];
    float    p0;                    /* P₀ = p0·I */
    uint32_t samples;
    uint32_t resets;
} rls_t;

typedef enum {
    RLS_UPDATED = 0,
    RLS_REJECTED,                   /* entrée non finie : aucune mise à jour */
    RLS_RESET,                      /* divergence : θ = θ₀, P = P₀ */
} rls_result_t;

void rls_init(rls_t *r, uint8_t n, float lambda, const float *theta0, float p0);
void rls_reset(rls_t *r);

/* Le contrôle des bornes physiques de x et y est fait par l'appelant ; ici, NaN et ±inf. */
rls_result_t rls_update(rls_t *r, const float *x, float y);

float rls_predict(const rls_t *r, const float *x);

bool rls_ready(const rls_t *r);
