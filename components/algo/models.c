#include <math.h>
#include "models.h"

#define HOUR_S      3600
#define PV_DERATE   0.85f
#define SLOT_H      0.5f
#define PV_P0       1.0f        /* covariance initiale : apprentissage rapide pendant le rodage */
#define BASE_P0     0.01f

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float models_hourly_at(const float *h, int n, int64_t t0, int32_t center_offset_s, int64_t t)
{
    if (n <= 0) {
        return NAN;
    }
    int64_t first = t0 + center_offset_s;
    int64_t last = first + (int64_t)(n - 1) * HOUR_S;
    /* Au-delà d'une demi-heure hors de la série : pas de donnée. */
    if (t < first - HOUR_S / 2 || t > last + HOUR_S / 2) {
        return NAN;
    }
    if (t <= first) {
        return h[0];
    }
    if (t >= last) {
        return h[n - 1];
    }
    int64_t rel = t - first;
    int i = (int)(rel / HOUR_S);
    float frac = (float)(rel - (int64_t)i * HOUR_S) / (float)HOUR_S;
    float a = h[i];
    float b = h[i + 1];
    if (!isfinite(a) || !isfinite(b)) {
        return NAN;
    }
    return a + (b - a) * frac;
}

void models_pv_init(rls_t *r, float kwc)
{
    const float th0[3] = { kwc * PV_DERATE * SLOT_H, 0.0f, 0.0f };
    rls_init(r, 3, MODELS_PV_LAMBDA, th0, PV_P0);
}

void models_pv_x(float gti, float temp, float x[3])
{
    x[0] = gti;
    x[1] = isfinite(temp) ? gti * (temp - 25.0f) : 0.0f;
    x[2] = 1.0f;
}

float models_pv_fallback(float kwc, float gti)
{
    return kwc * PV_DERATE * gti * SLOT_H;
}

float models_pv_max(float kwc)
{
    return 1.2f * kwc * 1000.0f * SLOT_H;
}

float models_pv_predict(const rls_t *r, float kwc, float gti, float temp)
{
    if (!isfinite(gti) || gti <= 0.0f) {
        return 0.0f;
    }
    float y;
    if (rls_ready(r)) {
        float x[3];
        models_pv_x(gti, temp, x);
        y = rls_predict(r, x);
    } else {
        y = models_pv_fallback(kwc, gti);
    }
    return isfinite(y) ? clampf(y, 0.0f, models_pv_max(kwc)) : 0.0f;
}

void models_base_init(rls_t *r)
{
    const float th0[3] = { 1.0f, 0.0f, 0.0f };
    rls_init(r, 3, MODELS_BASE_LAMBDA, th0, BASE_P0);
}

void models_base_x(float prof, float temp, float x[3])
{
    x[0] = prof;
    x[1] = isfinite(temp) && temp < MODELS_HEAT_REF_C ? MODELS_HEAT_REF_C - temp : 0.0f;
    x[2] = 1.0f;
}

float models_base_predict(const rls_t *r, float prof, float temp)
{
    float y = prof;
    if (rls_ready(r)) {
        float x[3];
        models_base_x(prof, temp, x);
        y = rls_predict(r, x);
    }
    return isfinite(y) ? clampf(y, 0.0f, MODELS_BASE_MAX_WH) : prof;
}
