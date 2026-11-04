#include <math.h>
#include "unity.h"
#include "models.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static const float H[4] = { 0.0f, 100.0f, 300.0f, 200.0f };
#define T0 1000000

static void test_preceding_mean_is_centered_half_hour_before(void)
{
    /* h[1] = moyenne de [T0, T0+3600] : centrée en T0 + 1800. */
    TEST_ASSERT_EQUAL_FLOAT(100.0f, models_hourly_at(H, 4, T0, -1800, T0 + 1800));
    /* Milieu entre h[1] et h[2] : T0 + 3600. */
    TEST_ASSERT_EQUAL_FLOAT(200.0f, models_hourly_at(H, 4, T0, -1800, T0 + 3600));
    /* Créneau 30 min [T0+3600, T0+5400[ : milieu T0+4500, aux trois quarts de h[1] (T0+1800)
     * vers h[2] (T0+5400). */
    TEST_ASSERT_EQUAL_FLOAT(250.0f, models_hourly_at(H, 4, T0, -1800, T0 + 4500));
}

static void test_instant_values(void)
{
    TEST_ASSERT_EQUAL_FLOAT(100.0f, models_hourly_at(H, 4, T0, 0, T0 + 3600));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, models_hourly_at(H, 4, T0, 0, T0 + 1800));
}

static void test_edges_and_outside(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, models_hourly_at(H, 4, T0, 0, T0 - 900));
    TEST_ASSERT_EQUAL_FLOAT(200.0f, models_hourly_at(H, 4, T0, 0, T0 + 3 * 3600 + 900));
    TEST_ASSERT_TRUE(isnan(models_hourly_at(H, 4, T0, 0, T0 - 3600)));
    TEST_ASSERT_TRUE(isnan(models_hourly_at(H, 4, T0, 0, T0 + 5 * 3600)));
    TEST_ASSERT_TRUE(isnan(models_hourly_at(H, 0, T0, 0, T0)));
    const float with_nan[2] = { 10.0f, NAN };
    TEST_ASSERT_TRUE(isnan(models_hourly_at(with_nan, 2, T0, 0, T0 + 1800)));
}

static void test_pv_fallback_matches_theta0(void)
{
    rls_t r;
    models_pv_init(&r, 3.0f);
    /* 3 kWc, 1000 W/m², 30 min, 0,85 → 1275 Wh. */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1275.0f, models_pv_fallback(3.0f, 1000.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1275.0f, models_pv_predict(&r, 3.0f, 1000.0f, 25.0f));
    float x[3];
    models_pv_x(1000.0f, 25.0f, x);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1275.0f, rls_predict(&r, x));    /* θ₀·x = repli */
}

static void test_pv_prediction_bounds(void)
{
    rls_t r;
    models_pv_init(&r, 3.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, models_pv_predict(&r, 3.0f, 0.0f, 10.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, models_pv_predict(&r, 3.0f, NAN, 10.0f));
    TEST_ASSERT_EQUAL_FLOAT(models_pv_max(3.0f), models_pv_predict(&r, 3.0f, 5000.0f, 25.0f));
}

/* Le modèle apprend un ombrage : la production réelle vaut 60 % du repli. */
static void test_pv_learns_shading(void)
{
    rls_t r;
    models_pv_init(&r, 3.0f);
    for (int i = 0; i < 300; i++) {
        float gti = 50.0f + (float)((i * 37) % 800);
        float temp = 5.0f + (float)((i * 11) % 25);
        float x[3];
        models_pv_x(gti, temp, x);
        rls_update(&r, x, 0.6f * models_pv_fallback(3.0f, gti));
    }
    TEST_ASSERT_TRUE(rls_ready(&r));
    TEST_ASSERT_FLOAT_WITHIN(15.0f, 0.6f * 1275.0f * 0.5f, models_pv_predict(&r, 3.0f, 500.0f, 15.0f));
}

static void test_base_uses_profile_until_ready(void)
{
    rls_t r;
    models_base_init(&r);
    TEST_ASSERT_EQUAL_FLOAT(250.0f, models_base_predict(&r, 250.0f, 5.0f));
    float x[3];
    models_base_x(250.0f, 5.0f, x);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, x[1]);
    models_base_x(250.0f, 20.0f, x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x[1]);
    models_base_x(250.0f, NAN, x);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, x[1]);
}

/* Chauffage électrique : +20 Wh par degré sous 15 °C. */
static void test_base_learns_heating(void)
{
    rls_t r;
    models_base_init(&r);
    for (int i = 0; i < 400; i++) {
        float prof = 150.0f + (float)((i * 13) % 200);
        float temp = -5.0f + (float)((i * 7) % 30);
        float x[3];
        models_base_x(prof, temp, x);
        rls_update(&r, x, prof + 20.0f * x[1]);
    }
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 200.0f + 20.0f * 10.0f, models_base_predict(&r, 200.0f, 5.0f));
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 200.0f, models_base_predict(&r, 200.0f, 20.0f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_preceding_mean_is_centered_half_hour_before);
    RUN_TEST(test_instant_values);
    RUN_TEST(test_edges_and_outside);
    RUN_TEST(test_pv_fallback_matches_theta0);
    RUN_TEST(test_pv_prediction_bounds);
    RUN_TEST(test_pv_learns_shading);
    RUN_TEST(test_base_uses_profile_until_ready);
    RUN_TEST(test_base_learns_heating);
    return UNITY_END();
}
