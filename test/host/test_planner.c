#include <math.h>
#include <string.h>
#include "unity.h"
#include "planner.h"

/* Plan calculé à 06:00 : créneaux 0–31 en HP (06:00–22:00), 32–47 en HC (22:00–06:00).
 * PV = P Wh sur les créneaux 6–17 (09:00–15:00), consommation de base 200 Wh partout.
 * B = déficit HP après le premier soleil = créneaux 18–31 → 14 × 200 = 2800 Wh. */
#define T0      (6 * 3600)
#define SHOWER  (20 * 3600)
#define E_CE    4375.0f     /* 2500 W × 105 min */

static plan_in_t in;

void setUp(void)
{
    memset(&in, 0, sizeof(in));
    in.t0 = T0;
    in.n = PLAN_SLOTS;
    in.soc = 10.0f;
    in.soc_min = 10.0f;
    in.soc_max = 100.0f;
    in.capa_kwh = 3.2f;
    in.eta = 0.88f;
    in.wear_eur_kwh = 0.08f;
    in.price_hp = 0.27f;
    in.price_hc = 0.20f;
    in.price_sell = 0.04f;
    in.e_ce_wh = E_CE;
    in.shower_at = SHOWER;
    for (int k = 0; k < PLAN_SLOTS; k++) {
        in.base_wh[k] = 200.0f;
        in.hc[k] = k >= 32;
    }
}

void tearDown(void)
{
}

static void sun(float p)
{
    for (int k = 6; k < 18; k++) {
        in.pv_wh[k] = p;
    }
}

static void test_low_sun_goes_to_battery_only(void)
{
    sun(300.0f);                    /* S = 12 × 100 = 1200 < B */
    plan_t p;
    planner_compute(&in, &p);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 1200.0f, p.S_wh);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 2800.0f, p.B_wh);
    TEST_ASSERT_EQUAL(CE_HC_ONLY, p.mode);
}

static void test_medium_sun_surplus_mode(void)
{
    sun(600.0f);                    /* S = 4800 : B ≤ S < B + E_ce */
    plan_t p;
    planner_compute(&in, &p);
    TEST_ASSERT_EQUAL(CE_SOLAR_SURPLUS, p.mode);
    /* Valeur batterie 0,27 × 0,88 − 0,08 = 0,1576 €/kWh < valeur chauffe-eau (prix HC) 0,20 €/kWh :
     * avec ces prix, le chauffe-eau passe avant la batterie (§9.1, ordre calculé, pas codé en dur). */
    TEST_ASSERT_TRUE(p.ce_first);
}

static void test_high_sun_early_mode(void)
{
    sun(900.0f);                    /* S = 8400 ≥ 2800 + 4375 */
    plan_t p;
    planner_compute(&in, &p);
    TEST_ASSERT_EQUAL(CE_SOLAR_EARLY, p.mode);
    TEST_ASSERT_TRUE(p.ce_first);
}

static void test_battery_already_full(void)
{
    in.soc = 100.0f;
    sun(600.0f);
    plan_t p;
    planner_compute(&in, &p);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.B_wh);
    TEST_ASSERT_EQUAL(CE_SOLAR_EARLY, p.mode);     /* S = 4800 ≥ 0 + 4375 */
}

static void test_no_forecast(void)
{
    plan_t p;
    planner_compute(&in, &p);       /* PV nul partout */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.S_wh);
    TEST_ASSERT_EQUAL(CE_HC_ONLY, p.mode);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, planner_soc_target(&p, T0 + 5 * 3600, 10.0f));
    in.n = 0;
    planner_compute(&in, &p);
    TEST_ASSERT_EQUAL(CE_HC_ONLY, p.mode);
}

static void test_soc_target_reaches_b_at_end_of_production(void)
{
    sun(600.0f);
    plan_t p;
    planner_compute(&in, &p);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, p.soc_target[5]);                        /* avant le soleil */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f + 2800.0f / 3200.0f * 90.0f, p.soc_target[17]);
    for (int k = 1; k < PLAN_SLOTS; k++) {
        TEST_ASSERT_TRUE(p.soc_target[k] >= p.soc_target[k - 1]);
    }
    /* Pendant le créneau 12, la cible est celle de la fin du créneau 11. */
    TEST_ASSERT_EQUAL_FLOAT(p.soc_target[11], planner_soc_target(&p, T0 + 12 * 1800 + 60, 10.0f));
    TEST_ASSERT_EQUAL_FLOAT(10.0f, planner_soc_target(&p, T0 + 60, 10.0f));    /* créneau 0 : SOC actuel */
}

static void test_value_order_from_prices(void)
{
    sun(600.0f);
    in.price_hc = 0.10f;            /* batterie 0,1576 €/kWh > chauffe-eau 0,10 */
    plan_t p;
    planner_compute(&in, &p);
    TEST_ASSERT_FALSE(p.ce_first);
    in.price_hp = 0.15f;            /* batterie 0,052 < chauffe-eau 0,10 */
    planner_compute(&in, &p);
    TEST_ASSERT_TRUE(p.ce_first);
}

static void test_surplus_before_shower(void)
{
    sun(600.0f);
    in.pv_wh[29] = 1200.0f;         /* créneau 20:30–21:00 : après les douches */
    plan_t p;
    planner_compute(&in, &p);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 4800.0f, p.S_shower_wh);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 5800.0f, p.S_wh);
}

/* ---------- Arbitrage réseau (§9.6) ---------- */

/* Nuit de 16 créneaux HC (prix variables), puis 32 créneaux HP à 0,253 €/kWh. Batterie vide, pas de soleil. */
static void arb_night(float hc_cheap, int n_cheap, float hc_rest, float hp)
{
    in.soc = in.soc_min;
    in.capa_kwh = 3.84f;
    in.wear_eur_kwh = 0.042f;
    for (int k = 0; k < PLAN_SLOTS; k++) {
        in.hc[k] = k < 16;
        in.price[k] = k < 16 ? (k < n_cheap ? hc_cheap : hc_rest) : hp;
        in.pv_wh[k] = 0.0f;
        in.base_wh[k] = 200.0f;
    }
}

/* Dimanche soir (HC week-end 0,157) puis lundi (HP semaine 0,253) : seule la partie avant minuit est rentable. */
static void test_arbitrage_sunday_night_cheap_slots_only(void)
{
    arb_night(0.157f, 3, 0.191f, 0.253f);
    arb_t a;
    planner_arbitrage(&in, 1600.0f, 1600.0f, &a);
    TEST_ASSERT_EQUAL_INT(0, a.hc_start);
    TEST_ASSERT_EQUAL_INT(16, a.hc_end);
    TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.253f - 0.157f / 0.88f - 0.042f, a.gain_kwh);
    TEST_ASSERT_TRUE(a.gain_eur > 0.0f);
    for (int k = 3; k < 16; k++) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, a.charge_wh[k]);      /* après minuit : HC semaine, non rentable */
    }
    /* 3 créneaux × 800 Wh, au pas de recherche près. */
    TEST_ASSERT_FLOAT_WITHIN(ARB_STEP_WH / 0.88f, 2400.0f, a.grid_wh);
    TEST_ASSERT_TRUE(a.grid_wh <= 2400.5f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, a.grid_wh * 0.88f, a.stored_wh);
    TEST_ASSERT_TRUE(a.soc_target > in.soc_min);
}

/* Semaine → semaine en saison haute : 0,253 − 0,191 / 0,88 − 0,042 < 0, aucune charge. */
static void test_arbitrage_not_profitable(void)
{
    arb_night(0.191f, 16, 0.191f, 0.253f);
    arb_t a;
    planner_arbitrage(&in, 1600.0f, 1600.0f, &a);
    TEST_ASSERT_TRUE(a.gain_kwh < 0.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.grid_wh);
    TEST_ASSERT_EQUAL_FLOAT(in.soc, a.soc_target);
}

/* Lendemain ensoleillé : le soleil remplit la batterie, on ne charge que le déficit du matin. */
static void test_arbitrage_sunny_day_charges_morning_only(void)
{
    arb_night(0.10f, 16, 0.10f, 0.30f);
    for (int k = 20; k < 32; k++) {
        in.pv_wh[k] = 1200.0f;                              /* +1000 Wh par créneau : batterie pleine */
    }
    arb_t a;
    planner_arbitrage(&in, 1600.0f, 1600.0f, &a);
    TEST_ASSERT_TRUE(a.gain_eur > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(60.0f, 800.0f, a.stored_wh);    /* 4 créneaux du matin × 200 Wh */
}

static void test_arbitrage_no_hc_in_horizon(void)
{
    for (int k = 0; k < PLAN_SLOTS; k++) {
        in.hc[k] = false;
        in.price[k] = 0.20f;
    }
    arb_t a;
    planner_arbitrage(&in, 1600.0f, 1600.0f, &a);
    TEST_ASSERT_EQUAL_INT(-1, a.hc_start);
    TEST_ASSERT_TRUE(isnan(a.gain_kwh));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, a.grid_wh);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_low_sun_goes_to_battery_only);
    RUN_TEST(test_medium_sun_surplus_mode);
    RUN_TEST(test_high_sun_early_mode);
    RUN_TEST(test_battery_already_full);
    RUN_TEST(test_no_forecast);
    RUN_TEST(test_soc_target_reaches_b_at_end_of_production);
    RUN_TEST(test_value_order_from_prices);
    RUN_TEST(test_arbitrage_sunday_night_cheap_slots_only);
    RUN_TEST(test_arbitrage_not_profitable);
    RUN_TEST(test_arbitrage_sunny_day_charges_morning_only);
    RUN_TEST(test_arbitrage_no_hc_in_horizon);
    RUN_TEST(test_surplus_before_shower);
    return UNITY_END();
}
