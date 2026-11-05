#include <math.h>
#include "unity.h"
#include "rls.h"
#include "profile.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* Générateur pseudo-aléatoire déterministe (LCG), valeurs dans [0, 1[. */
static uint32_t seed = 12345;
static float rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8) / 16777216.0f;
}

static void test_converges_to_known_coefficients(void)
{
    const float th0[3] = { 0, 0, 0 };
    rls_t r;
    rls_init(&r, 3, 0.998f, th0, 1000.0f);
    /* y = 1,2·a − 0,004·b + 15, bruit ±1 */
    for (int i = 0; i < 2000; i++) {
        float x[3] = { rnd() * 800.0f, rnd() * 800.0f * 20.0f - 4000.0f, 1.0f };
        float y = 1.2f * x[0] - 0.004f * x[1] + 15.0f + (rnd() - 0.5f) * 2.0f;
        TEST_ASSERT_NOT_EQUAL(RLS_RESET, rls_update(&r, x, y));
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.2f, r.theta[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.004f, r.theta[1]);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 15.0f, r.theta[2]);
    TEST_ASSERT_TRUE(rls_ready(&r));
}

/* Avec λ < 1, le modèle suit un changement de régime (ex. ombrage saisonnier). */
static void test_lambda_tracks_change(void)
{
    const float th0[2] = { 0, 0 };
    rls_t fast, slow;
    rls_init(&fast, 2, 0.95f, th0, 1000.0f);
    rls_init(&slow, 2, 1.0f, th0, 1000.0f);
    for (int i = 0; i < 400; i++) {
        float x[2] = { rnd() * 100.0f, 1.0f };
        float y = (i < 200 ? 2.0f : 1.0f) * x[0];
        rls_update(&fast, x, y);
        rls_update(&slow, x, y);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 1.0f, fast.theta[0]);
    TEST_ASSERT_TRUE(fabsf(slow.theta[0] - 1.0f) > 0.2f);
}

static void test_rejects_nan_and_inf(void)
{
    const float th0[2] = { 0.5f, 0 };
    rls_t r;
    rls_init(&r, 2, 0.998f, th0, 100.0f);
    float x[2] = { NAN, 1.0f };
    TEST_ASSERT_EQUAL(RLS_REJECTED, rls_update(&r, x, 10.0f));
    x[0] = 1.0f;
    TEST_ASSERT_EQUAL(RLS_REJECTED, rls_update(&r, x, INFINITY));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, r.theta[0]);
    TEST_ASSERT_EQUAL_UINT32(0, r.samples);
}

static void test_resets_on_divergence(void)
{
    const float th0[2] = { 0.5f, 0 };
    rls_t r;
    rls_init(&r, 2, 0.5f, th0, 100.0f);     /* λ très faible : P explose sans excitation */
    float x[2] = { 0.0f, 0.0f };
    rls_result_t res = RLS_UPDATED;
    for (int i = 0; i < 64 && res != RLS_RESET; i++) {
        res = rls_update(&r, x, 1.0f);
    }
    TEST_ASSERT_EQUAL(RLS_RESET, res);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, r.theta[0]);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, r.P[0][0]);
    TEST_ASSERT_EQUAL_UINT32(1, r.resets);
    TEST_ASSERT_EQUAL_UINT32(0, r.samples);
}

static void test_not_ready_before_96_samples(void)
{
    const float th0[1] = { 1 };
    rls_t r;
    rls_init(&r, 1, 0.998f, th0, 10.0f);
    float x[1] = { 1.0f };
    for (int i = 0; i < 95; i++) {
        rls_update(&r, x, 1.0f);
    }
    TEST_ASSERT_FALSE(rls_ready(&r));
    rls_update(&r, x, 1.0f);
    TEST_ASSERT_TRUE(rls_ready(&r));
}

static void test_profile_ema(void)
{
    profile_t p;
    profile_init(&p);
    TEST_ASSERT_EQUAL_FLOAT(PROFILE_DEFAULT_WH, profile_get(&p, 0, 10));
    TEST_ASSERT_TRUE(profile_update(&p, 0, 10, 200.0f));
    TEST_ASSERT_EQUAL_FLOAT(200.0f, profile_get(&p, 0, 10));
    TEST_ASSERT_TRUE(profile_update(&p, 0, 10, 300.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 210.0f, profile_get(&p, 0, 10));
    TEST_ASSERT_FALSE(profile_update(&p, 0, 10, NAN));
    TEST_ASSERT_FALSE(profile_update(&p, 0, 48, 100.0f));
    TEST_ASSERT_FALSE(profile_update(&p, 2, 0, 100.0f));
    /* Créneau du week-end jamais mesuré : celui de la semaine. */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 210.0f, profile_get(&p, 1, 10));
    TEST_ASSERT_EQUAL_INT(1, profile_learned_slots(&p));
    TEST_ASSERT_EQUAL_INT(1, profile_day_type(0));
    TEST_ASSERT_EQUAL_INT(0, profile_day_type(3));
    TEST_ASSERT_EQUAL_INT(1, profile_day_type(6));
}

static void test_profile_fallback_chain(void)
{
    profile_t p;
    profile_init(&p);
    TEST_ASSERT_EQUAL_FLOAT(PROFILE_DEFAULT_WH, profile_get(&p, 0, 5));     /* profil vide */
    profile_update(&p, 0, 20, 100.0f);
    profile_update(&p, 0, 30, 500.0f);
    profile_update(&p, 1, 40, 900.0f);
    TEST_ASSERT_EQUAL_FLOAT(100.0f, profile_get(&p, 1, 20));                /* autre type de jour */
    TEST_ASSERT_EQUAL_FLOAT(300.0f, profile_get(&p, 0, 5));                 /* moyenne semaine (100, 500) */
    TEST_ASSERT_EQUAL_FLOAT(900.0f, profile_get(&p, 1, 5));                 /* moyenne week-end (900) */
    TEST_ASSERT_EQUAL_INT(3, profile_learned_slots(&p));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_converges_to_known_coefficients);
    RUN_TEST(test_lambda_tracks_change);
    RUN_TEST(test_rejects_nan_and_inf);
    RUN_TEST(test_resets_on_divergence);
    RUN_TEST(test_not_ready_before_96_samples);
    RUN_TEST(test_profile_ema);
    RUN_TEST(test_profile_fallback_chain);
    return UNITY_END();
}
