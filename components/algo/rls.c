#include <math.h>
#include <string.h>
#include "rls.h"

void rls_reset(rls_t *r)
{
    memcpy(r->theta, r->theta0, sizeof(r->theta));
    memset(r->P, 0, sizeof(r->P));
    for (int i = 0; i < r->n; i++) {
        r->P[i][i] = r->p0;
    }
    r->samples = 0;
}

void rls_init(rls_t *r, uint8_t n, float lambda, const float *theta0, float p0)
{
    memset(r, 0, sizeof(*r));
    r->n = n > RLS_N_MAX ? RLS_N_MAX : n;
    r->lambda = lambda;
    r->p0 = p0;
    for (int i = 0; i < r->n; i++) {
        r->theta0[i] = theta0[i];
    }
    rls_reset(r);
}

static bool all_finite(const float *v, int n)
{
    for (int i = 0; i < n; i++) {
        if (!isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

rls_result_t rls_update(rls_t *r, const float *x, float y)
{
    const int n = r->n;
    if (!isfinite(y) || !all_finite(x, n)) {
        return RLS_REJECTED;
    }

    float px[RLS_N_MAX];            /* P·x */
    float denom = r->lambda;
    for (int i = 0; i < n; i++) {
        px[i] = 0.0f;
        for (int j = 0; j < n; j++) {
            px[i] += r->P[i][j] * x[j];
        }
        denom += x[i] * px[i];
    }
    if (!(denom > 0.0f) || !isfinite(denom)) {
        rls_reset(r);
        r->resets++;
        return RLS_RESET;
    }

    float k[RLS_N_MAX];
    float e = y;
    for (int i = 0; i < n; i++) {
        k[i] = px[i] / denom;
        e -= r->theta[i] * x[i];
    }

    /* xᵀ·P = (P·x)ᵀ, P étant symétrique. */
    float trace = 0.0f;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        r->theta[i] += k[i] * e;
        ok = ok && isfinite(r->theta[i]);
        for (int j = 0; j < n; j++) {
            r->P[i][j] = (r->P[i][j] - k[i] * px[j]) / r->lambda;
        }
    }
    /* Symétrisation contre la dérive numérique en simple précision. */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            float s = 0.5f * (r->P[i][j] + r->P[j][i]);
            r->P[i][j] = s;
            r->P[j][i] = s;
        }
        trace += r->P[i][i];
    }

    if (!ok || !isfinite(trace) || trace > RLS_TRACE_MAX) {
        rls_reset(r);
        r->resets++;
        return RLS_RESET;
    }
    r->samples++;
    return RLS_UPDATED;
}

float rls_predict(const rls_t *r, const float *x)
{
    float y = 0.0f;
    for (int i = 0; i < r->n; i++) {
        y += r->theta[i] * x[i];
    }
    return y;
}

bool rls_ready(const rls_t *r)
{
    return r->samples >= RLS_MIN_SAMPLES;
}
