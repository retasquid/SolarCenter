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

/* Une heure à puissances constantes, Envoy muet puis reconstitué : mêmes résultats que si tout était mesuré. */
static void check_gap_equivalence(flows_in_t in, float price)
{
    energy_acc_t full = { 0 }, gap = { 0 };
    gap_acc_t g = { 0 };
    flows_in_t no_pv = in;
    no_pv.p_pv = 0.0f;
    for (int t = 0; t < 3600; t += 2) {
        stats_integrate(&full, &in, &in, 2.0f, true, price, SELL, HC);
        stats_integrate(&gap, &no_pv, &no_pv, 2.0f, false, price, SELL, HC);
        stats_gap_integrate(&g, &no_pv, &no_pv, 2.0f, true, price, HC);
    }
    energy_acc_t d;
    stats_gap_fill(&g, in.p_pv, SELL, &d);      /* 1 h à p_pv W = p_pv Wh */
    stats_add(&gap, &d);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_pv, gap.e_pv);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_home, gap.e_home);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_batt_ch, gap.e_batt_ch);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_batt_dis, gap.e_batt_dis);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_ce, gap.e_ce);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_ce_local, gap.e_ce_local);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_pv_home, gap.e_pv_home);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_pv_batt, gap.e_pv_batt);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_pv_grid, gap.e_pv_grid);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_grid_home, gap.e_grid_home);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_grid_batt, gap.e_grid_batt);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, full.e_batt_home, gap.e_batt_home);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, full.c_real, gap.c_real);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, full.c_no_batt, gap.c_no_batt);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, full.c_no_install, gap.c_no_install);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, full.hc_ce_value, gap.hc_ce_value);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, gap.secs_full);       /* reste « incomplet » : pas d'apprentissage */
}

static void test_gap_sunny_export_and_charge(void)
{
    /* PV 2000 W, maison 1500 W dont chauffe-eau 1000 W, batterie en charge 300 W, export 200 W. */
    check_gap_equivalence((flows_in_t){ .p_pv = 2000, .p_grid = -200, .p_batt = -300, .p_ce = 1000 }, HP);
}

static void test_gap_evening_discharge(void)
{
    /* Pas de PV, batterie 400 W vers la maison, import 600 W. */
    check_gap_equivalence((flows_in_t){ .p_pv = 0, .p_grid = 600, .p_batt = 400, .p_ce = 0 }, HP);
}

static void test_gap_charge_from_grid(void)
{
    /* PV 500 W, batterie en charge 800 W (dont 300 W du réseau), maison 0 W. */
    check_gap_equivalence((flows_in_t){ .p_pv = 500, .p_grid = 300, .p_batt = -800, .p_ce = 0 }, HC);
}

/* SolarFlow inconnu pendant le trou : seul le PV est ajouté, la maison et les gains restent neutres. */
static void test_gap_battery_unknown_adds_pv_only(void)
{
    gap_acc_t g = { 0 };
    flows_in_t in = { .p_grid = -200 };
    stats_gap_integrate(&g, &in, &in, 2.0f, true, HP, HC);
    stats_gap_integrate(&g, &in, &in, 2.0f, false, HP, HC);
    energy_acc_t d;
    stats_gap_fill(&g, 300.0f, SELL, &d);
    TEST_ASSERT_EQUAL_FLOAT(300.0f, d.e_pv);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.e_home);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.c_no_install);
    stats_gap_fill(&g, -5.0f, SELL, &d);        /* compteur incohérent : jamais négatif */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, d.e_pv);
}

/* ---------- Rendement réel de la batterie ---------- */

/* Batterie simulée : 40 Wh par % de SOC, rendements de charge et de décharge connus, SOC entier. */
typedef struct {
    float stored;       /* Wh */
    float p_prev;
    int64_t now;
} sim_batt_t;

static int sim_soc(const sim_batt_t *b)
{
    return (int)lroundf(b->stored / 40.0f);
}

/* p > 0 décharge (côté AC), < 0 charge ; `seconds` par pas de 2 s. Retourne le nombre de cycles mesurés. */
static int sim_run(eta_learn_t *e, sim_batt_t *b, float p_req, int seconds, bool ok)
{
    int cycles = 0;
    for (int t = 0; t < seconds; t += 2) {
        /* Butées du SolarFlow : plus de décharge à 10 %, plus de charge à 100 %. */
        float p = p_req > 0 && b->stored <= 400.0f ? 0.0f : (p_req < 0 && b->stored >= 4000.0f ? 0.0f : p_req);
        float wh = p * 2.0f / 3600.0f;
        b->stored -= p > 0 ? wh / 0.88f : wh * 0.90f;      /* η_charge 0,90, η_décharge 0,88 */
        b->now += 2;
        cycles += eta_learn_step(e, ok, b->p_prev, p, 2.0f, sim_soc(b), b->now) ? 1 : 0;
        b->p_prev = p;
    }
    return cycles;
}

static void test_eta_full_cycle(void)
{
    eta_learn_t e;
    eta_learn_init(&e);
    sim_batt_t b = { .stored = 400.0f };                    /* 10 % */
    TEST_ASSERT_EQUAL_INT(0, sim_run(&e, &b, -1000.0f, 3 * 3600, true));   /* 3 kWh entrés */
    TEST_ASSERT_EQUAL_INT(0, sim_run(&e, &b, 0.0f, 3600, true));
    TEST_ASSERT_EQUAL_INT(1, sim_run(&e, &b, 1000.0f, 3 * 3600, true));
    TEST_ASSERT_FLOAT_WITHIN(0.015f, 0.90f * 0.88f, e.last);
    TEST_ASSERT_EQUAL_UINT16(1, e.samples);
    TEST_ASSERT_EQUAL_FLOAT(0.88f, eta_learn_value(&e, 0.88f));   /* pas encore 3 cycles : config */
}

static void test_eta_small_swing_waits_for_enough_energy(void)
{
    eta_learn_t e;
    eta_learn_init(&e);
    sim_batt_t b = { .stored = 2000.0f };                   /* 50 % */
    /* Petit aller-retour (0,5 kWh) : rien de mesuré, le cycle continue. */
    TEST_ASSERT_EQUAL_INT(0, sim_run(&e, &b, -500.0f, 3600, true));
    TEST_ASSERT_EQUAL_INT(0, sim_run(&e, &b, 500.0f, 3600, true));
    TEST_ASSERT_EQUAL_INT(0, sim_run(&e, &b, -1000.0f, 2 * 3600, true));
    TEST_ASSERT_EQUAL_INT(1, sim_run(&e, &b, 1000.0f, 3 * 3600, true));
    TEST_ASSERT_FLOAT_WITHIN(0.015f, 0.90f * 0.88f, e.last);
}

static void test_eta_gap_aborts_cycle(void)
{
    eta_learn_t e;
    eta_learn_init(&e);
    sim_batt_t b = { .stored = 400.0f };
    sim_run(&e, &b, -1000.0f, 3 * 3600, true);
    sim_run(&e, &b, 1000.0f, 600, false);                   /* SolarFlow muet : énergie inconnue */
    TEST_ASSERT_EQUAL_INT(0, sim_run(&e, &b, 1000.0f, 3 * 3600, true));
    TEST_ASSERT_EQUAL_UINT16(0, e.samples);
}

static void test_eta_moving_average_and_ready(void)
{
    eta_learn_t e;
    eta_learn_init(&e);
    sim_batt_t b = { .stored = 400.0f };
    for (int i = 0; i < 3; i++) {
        sim_run(&e, &b, -1000.0f, 3 * 3600, true);
        sim_run(&e, &b, 1000.0f, 3 * 3600, true);
        sim_run(&e, &b, 1000.0f, 600, true);                /* en butée basse : SOC qui ne bouge plus */
    }
    TEST_ASSERT_TRUE(e.samples >= 3);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.792f, eta_learn_value(&e, 0.88f));
    /* Cycle de plus de 72 h : abandonné, rien d'appris. */
    uint16_t n = e.samples;
    sim_run(&e, &b, -1000.0f, 3600, true);
    sim_run(&e, &b, 0.0f, 80 * 3600, true);
    sim_run(&e, &b, 1000.0f, 3600, true);
    TEST_ASSERT_EQUAL_UINT16(n, e.samples);
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
    RUN_TEST(test_gap_sunny_export_and_charge);
    RUN_TEST(test_gap_evening_discharge);
    RUN_TEST(test_gap_charge_from_grid);
    RUN_TEST(test_gap_battery_unknown_adds_pv_only);
    RUN_TEST(test_eta_full_cycle);
    RUN_TEST(test_eta_small_swing_waits_for_enough_energy);
    RUN_TEST(test_eta_gap_aborts_cycle);
    RUN_TEST(test_eta_moving_average_and_ready);
    return UNITY_END();
}
