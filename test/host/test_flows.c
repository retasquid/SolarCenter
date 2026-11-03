#include <math.h>
#include "unity.h"
#include "flows.h"
#include "sanity.h"

#define EPS 0.01f

void setUp(void)
{
}

void tearDown(void)
{
}

static flows_t split(float pv, float grid, float batt, float ce)
{
    const flows_in_t in = { .p_pv = pv, .p_grid = grid, .p_batt = batt, .p_ce = ce };
    flows_t f;
    flows_split(&in, &f);
    return f;
}

/* Conservation : chaque nœud reçoit ou fournit exactement sa puissance. */
static void assert_conservation(float pv, float grid, float batt, const flows_t *f)
{
    float charge = batt < 0 ? -batt : 0;
    float dis = batt > 0 ? batt : 0;
    float imp = grid > 0 ? grid : 0;
    float exp_ = grid < 0 ? -grid : 0;
    TEST_ASSERT_FLOAT_WITHIN(EPS, pv, f->pv_home + f->pv_batt + f->pv_grid);
    TEST_ASSERT_FLOAT_WITHIN(EPS, f->p_home, f->pv_home + f->batt_home + f->grid_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, charge, f->pv_batt + f->grid_batt);
    TEST_ASSERT_FLOAT_WITHIN(EPS, dis, f->batt_home + f->batt_grid);
    TEST_ASSERT_FLOAT_WITHIN(EPS, imp - exp_, f->grid_home + f->grid_batt - f->pv_grid - f->batt_grid);
}

static void assert_non_negative(const flows_t *f)
{
    const float *v = (const float *)f;
    for (unsigned i = 0; i < sizeof(*f) / sizeof(float); i++) {
        TEST_ASSERT_TRUE(v[i] >= 0.0f);
    }
}

static void test_sunny_surplus_charges_and_exports(void)
{
    /* PV 3000, maison 800, batterie charge 1500, export 700. */
    flows_t f = split(3000, -700, -1500, 0);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 800, f.p_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 800, f.pv_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1500, f.pv_batt);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 700, f.pv_grid);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0, f.grid_home);
    assert_conservation(3000, -700, -1500, &f);
    assert_non_negative(&f);
}

static void test_evening_battery_and_grid_feed_home(void)
{
    /* Pas de PV, maison 1200 : batterie 800 + réseau 400. */
    flows_t f = split(0, 400, 800, 0);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1200, f.p_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 800, f.batt_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 400, f.grid_home);
    assert_conservation(0, 400, 800, &f);
    assert_non_negative(&f);
}

static void test_battery_charging_from_grid(void)
{
    /* Nuit, arbitrage : maison 300, charge 1000 depuis le réseau. */
    flows_t f = split(0, 1300, -1000, 0);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 300, f.p_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1000, f.grid_batt);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 300, f.grid_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0, f.pv_batt);
    assert_conservation(0, 1300, -1000, &f);
    assert_non_negative(&f);
}

static void test_partial_pv_charge_with_grid_complement(void)
{
    /* PV 500, maison 400, charge 600 : 100 du PV, 500 du réseau. */
    flows_t f = split(500, 500, -600, 0);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 400, f.pv_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 100, f.pv_batt);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 500, f.grid_batt);
    assert_conservation(500, 500, -600, &f);
    assert_non_negative(&f);
}

static void test_battery_discharge_exceeding_home(void)
{
    flows_t f = split(0, -200, 700, 0);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 500, f.batt_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 200, f.batt_grid);
    assert_conservation(0, -200, 700, &f);
    assert_non_negative(&f);
}

static void test_water_heater_local_share(void)
{
    /* Maison 3000 dont chauffe-eau 2500 ; local = PV 2000 + batt 0 sur 3000. */
    flows_t f = split(2000, 1000, 0, 2500);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2500, f.home_ce);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 500, f.p_base);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2500.0f * 2000.0f / 3000.0f, f.ce_local);
}

static void test_inconsistent_readings_stay_non_negative(void)
{
    /* Lectures désynchronisées : p_home calculé négatif. */
    flows_t f = split(100, -900, 0, 50);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0, f.p_home);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0, f.ce_local);
    assert_non_negative(&f);
}

static void test_sanity_bounds(void)
{
    TEST_ASSERT_TRUE(sanity_ok(QTY_GRID, -14999, 0));
    TEST_ASSERT_FALSE(sanity_ok(QTY_GRID, 15000, 0));
    TEST_ASSERT_TRUE(sanity_ok(QTY_PV, 3500, 3.0f));
    TEST_ASSERT_FALSE(sanity_ok(QTY_PV, 3601, 3.0f));   /* 3600 exactement : arrondi flottant */
    TEST_ASSERT_FALSE(sanity_ok(QTY_PV, -1, 3.0f));
    TEST_ASSERT_FALSE(sanity_ok(QTY_BATT, -3000, 0));
    TEST_ASSERT_TRUE(sanity_ok(QTY_CE, 0, 0));
    TEST_ASSERT_FALSE(sanity_ok(QTY_CE, 4000, 0));
    TEST_ASSERT_TRUE(sanity_ok(QTY_SOC, 100, 0));
    TEST_ASSERT_FALSE(sanity_ok(QTY_SOC, 100.5f, 0));
    TEST_ASSERT_FALSE(sanity_ok(QTY_GRID, NAN, 0));
    TEST_ASSERT_FALSE(sanity_ok(QTY_SOC, INFINITY, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sunny_surplus_charges_and_exports);
    RUN_TEST(test_evening_battery_and_grid_feed_home);
    RUN_TEST(test_battery_charging_from_grid);
    RUN_TEST(test_partial_pv_charge_with_grid_complement);
    RUN_TEST(test_battery_discharge_exceeding_home);
    RUN_TEST(test_water_heater_local_share);
    RUN_TEST(test_inconsistent_readings_stay_non_negative);
    RUN_TEST(test_sanity_bounds);
    return UNITY_END();
}
