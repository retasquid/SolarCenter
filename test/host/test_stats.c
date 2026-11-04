#include <math.h>
#include <string.h>
#include "unity.h"
#include "stats.h"

#define HP 0.27f
#define HC 0.20f
#define SELL 0.04f

void setUp(void)
{
}

void tearDown(void)
{
}

/* Intègre une puissance constante pendant `seconds` par pas de 2 s. */
static void run(energy_acc_t *acc, flows_in_t in, int seconds, float price)
{
    for (int t = 0; t < seconds; t += 2) {
        stats_integrate(acc, &in, &in, 2.0f, true, price, SELL, HC);
    }
}

static void test_constant_import_one_hour(void)
{
    energy_acc_t a = { 0 };
    run(&a, (flows_in_t){ .p_grid = 1000 }, 3600, HP);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1000.0f, a.e_import);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1000.0f, a.e_home);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.27f, a.c_real);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.27f, a.c_no_install);
}

static void test_trapezoid_and_dt_guard(void)
{
    energy_acc_t a = { 0 };
    flows_in_t p0 = { .p_grid = 0 }, p1 = { .p_grid = 3600 };
    TEST_ASSERT_TRUE(stats_integrate(&a, &p0, &p1, 2.0f, true, HP, SELL, HC));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, a.e_import);   /* (0 + 3600)/2 × 2 s = 1 Wh */
    TEST_ASSERT_FALSE(stats_integrate(&a, &p0, &p1, 10.5f, true, HP, SELL, HC));
    TEST_ASSERT_FALSE(stats_integrate(&a, &p0, &p1, 0.0f, true, HP, SELL, HC));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, a.e_import);
}

/* Shelly seul valide : import/export et coût réel comptés, gains neutres. */
static void test_partial_integration_grid_only(void)
{
    energy_acc_t a = { 0 };
    flows_in_t in = { .p_pv = 5000, .p_grid = 1000, .p_batt = 800 };    /* PV et batterie non fiables */
    for (int t = 0; t < 3600; t += 2) {
        stats_integrate(&a, &in, &in, 2.0f, false, HP, SELL, HC);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 1000.0f, a.e_import);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.e_pv);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.e_home);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 3600.0f, a.secs_grid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.secs_full);
    rates_t r;
    stats_rates(&a, 0.05f, &r);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, r.gain_batt);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, r.gain_total);
}

/* Énergies par flux : conservation sur une heure mixte. */
static void test_flow_energies_conserve(void)
{
    energy_acc_t a = { 0 };
    run(&a, (flows_in_t){ .p_pv = 3000, .p_grid = -700, .p_batt = -1500 }, 1800, HP);
    run(&a, (flows_in_t){ .p_pv = 0, .p_grid = 400, .p_batt = 800 }, 1800, HP);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, a.e_pv, a.e_pv_home + a.e_pv_batt + a.e_pv_grid);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, a.e_home, a.e_pv_home + a.e_batt_home + a.e_grid_home);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, a.e_batt_ch, a.e_pv_batt + a.e_grid_batt);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 750.0f, a.e_pv_batt);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 400.0f, a.e_batt_home);
}

static void test_known_rates(void)
{
    energy_acc_t a = { 0 };
    /* 1 h : PV 2000, maison 1000, export 1000. */
    run(&a, (flows_in_t){ .p_pv = 2000, .p_grid = -1000 }, 3600, HP);
    /* 1 h : PV 0, maison 1000 importés. */
    run(&a, (flows_in_t){ .p_grid = 1000 }, 3600, HP);
    rates_t r;
    stats_rates(&a, 0.0f, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, r.autoconso);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5f, r.autoprod);
}

static void test_no_battery_activity_gives_zero_gain(void)
{
    energy_acc_t a = { 0 };
    run(&a, (flows_in_t){ .p_pv = 1500, .p_grid = -300 }, 3600, HP);
    run(&a, (flows_in_t){ .p_grid = 800 }, 3600, HC);
    rates_t r;
    stats_rates(&a, 0.05f, &r);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, r.gain_batt);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, r.gain_batt_net);
}

/* Journée HP typique : la batterie stocke 1 kWh de surplus à midi et le restitue le soir. */
static void test_battery_gain_sign_typical_day(void)
{
    energy_acc_t a = { 0 };
    run(&a, (flows_in_t){ .p_pv = 2000, .p_grid = 0, .p_batt = -1000 }, 3600, HP);
    run(&a, (flows_in_t){ .p_grid = 0, .p_batt = 1000 }, 3600, HP);
    rates_t r;
    stats_rates(&a, 0.05f, &r);
    /* Sans batterie : export 1 kWh (0,04 €), import 1 kWh (0,27 €) → 0,23 € ; réel : 0 €. */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.23f, r.gain_batt);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.18f, r.gain_batt_net);
    TEST_ASSERT_TRUE(r.gain_batt > 0.0f);
}

static void test_wear_cost(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1000.0f / (6000.0f * 3.2f), stats_wear_cost(1000, 6000, 3.2f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, stats_wear_cost(0, 6000, 3.2f));
}

static void test_ce_routing_value(void)
{
    energy_acc_t a = { 0 };
    /* Chauffe-eau 2000 W entièrement solaire pendant 1 h. */
    run(&a, (flows_in_t){ .p_pv = 2500, .p_grid = -500, .p_ce = 2000 }, 3600, HP);
    rates_t r;
    stats_rates(&a, 0.0f, &r);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 2000.0f, a.e_ce_local);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f * HC, r.gain_ce);
}

static roi_params_t roi_params(void)
{
    roi_params_t p = {
        .kwc = 3.0f, .yield_kwh_kwc = 1200.0f, .capa_kwh = 3.2f, .eta = 0.88f,
        .price_hp = HP, .price_sell = SELL, .ratio_surplus = 0.5f,
        .month_frac = { 3, 5, 8, 10.5f, 12, 13, 13.5f, 12, 9, 6.5f, 4, 3.5f },
    };
    for (int m = 0; m < 12; m++) {
        p.hist_surplus_day[m] = -1.0f;
    }
    return p;
}

/* Estimation sans observation pour un mois : jours × min(capa, surplus/j) × (η·HP − revente). */
static float model_month(const roi_params_t *p, int m, int year)
{
    int d = stats_days_in_month(year, m + 1);
    float s = p->kwc * p->yield_kwh_kwc * p->month_frac[m] / 100.0f * p->ratio_surplus / d;
    return d * fminf(s, p->capa_kwh) * (p->eta * p->price_hp - p->price_sell);
}

static void test_roi_three_months_observed(void)
{
    roi_params_t p = roi_params();
    roi_month_t m[12] = { 0 };
    for (int i = 5; i < 8; i++) {   /* juin, juillet, août complets */
        m[i] = (roi_month_t){ .observed = true, .gain_batt_net = 15.0f,
                              .days_observed = (uint8_t)stats_days_in_month(2026, i + 1) };
    }
    float expected = 45.0f;
    for (int i = 0; i < 12; i++) {
        if (i < 5 || i >= 8) expected += model_month(&p, i, 2026);
    }
    roi_t r;
    stats_roi(m, &p, 1000.0f, 2026, &r);
    TEST_ASSERT_EQUAL_INT(3, r.months_observed);
    TEST_ASSERT_TRUE(r.estimate);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, expected, r.annual_gain);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1000.0f / expected, r.payback_years);
}

static void test_roi_thirteen_months_is_not_estimate(void)
{
    /* 13 mois d'historique : les 12 derniers mois sont tous observés. */
    roi_params_t p = roi_params();
    roi_month_t m[12];
    for (int i = 0; i < 12; i++) {
        m[i] = (roi_month_t){ .observed = true, .gain_batt_net = 10.0f,
                              .days_observed = (uint8_t)stats_days_in_month(2027, i + 1) };
    }
    roi_t r;
    stats_roi(m, &p, 600.0f, 2027, &r);
    TEST_ASSERT_FALSE(r.estimate);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 120.0f, r.annual_gain);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, r.payback_years);
}

static void test_roi_partial_month_extrapolated(void)
{
    roi_params_t p = roi_params();
    roi_month_t m[12] = { 0 };
    m[0] = (roi_month_t){ .observed = true, .gain_batt_net = 1.0f, .days_observed = 10 };
    roi_t r;
    stats_roi(m, &p, 1000.0f, 2026, &r);
    float expected = 3.1f;
    for (int i = 1; i < 12; i++) expected += model_month(&p, i, 2026);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, expected, r.annual_gain);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_constant_import_one_hour);
    RUN_TEST(test_trapezoid_and_dt_guard);
    RUN_TEST(test_partial_integration_grid_only);
    RUN_TEST(test_flow_energies_conserve);
    RUN_TEST(test_known_rates);
    RUN_TEST(test_no_battery_activity_gives_zero_gain);
    RUN_TEST(test_battery_gain_sign_typical_day);
    RUN_TEST(test_wear_cost);
    RUN_TEST(test_ce_routing_value);
    RUN_TEST(test_roi_three_months_observed);
    RUN_TEST(test_roi_thirteen_months_is_not_estimate);
    RUN_TEST(test_roi_partial_month_extrapolated);
    return UNITY_END();
}
