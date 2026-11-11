#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "regulator.h"

#define H(h)        ((int64_t)(h) * 3600)
#define HM(h, m)    ((int64_t)(h) * 3600 + (int64_t)(m) * 60)
#define DAY         H(24)
#define E_FULL      4375.0f     /* 2500 W × 105 min */

static reg_cfg_t cfg;
static reg_state_t st;
static reg_input_t in;
static reg_output_t out;

void setUp(void)
{
    cfg = (reg_cfg_t){
        .p_charge_max = 1600, .p_discharge_max = 1600, .soc_min = 10, .soc_max = 100,
        .solar_on_w = 1500, .min_on_s = 600, .min_off_s = 300,
        .wh = { .power_w = 2500, .full_heat_min = 105, .forecast_margin = 0.7f,
                .start_margin_min = 15, .catchup_hp = true },
    };
    regulator_init(&st, &cfg, H(12));
    in = (reg_input_t){
        .cfg = &cfg, .now = H(12), .dt_s = 5, .time_ok = true, .hc = false,
        .grid_ok = true, .batt_ok = true, .soc = 50, .ce_ok = true,
        .mode = CE_SOLAR_SURPLUS, .soc_target = 40,
        .last_shower = HM(20, 0) - DAY, .next_shower = HM(20, 0),
    };
    st.wh.cycle_start = in.last_shower;
}

void tearDown(void)
{
}

static void step(void)
{
    regulator_step(&in, &st, &out);
}

/* Avance de `seconds` par pas de 5 s ; relay_on suit la commande et le chauffe-eau consomme sa
 * puissance nominale quand il est alimenté (sinon le régulateur conclurait à une coupure du thermostat). */
static void run(int seconds)
{
    for (int t = 0; t < seconds; t += 5) {
        in.relay_on = st.relay_cmd;
        in.p_ce = st.relay_cmd ? cfg.wh.power_w : 0.0f;
        step();
        in.now += 5;
    }
}

/* ---------- Batterie ---------- */

static void test_zero_grid_tracking_with_ramp(void)
{
    in.p_grid = 900;
    step();
    TEST_ASSERT_EQUAL_FLOAT(REG_RAMP_W, out.batt_sp);   /* rampe ±600 W par pas */
    TEST_ASSERT_TRUE(out.batt_limits & RB_LIM_RAMP);
    in.p_batt = 600;
    in.p_grid = 300;
    step();
    TEST_ASSERT_EQUAL_FLOAT(900, out.batt_sp);          /* p_batt + p_grid */
}

static void test_deadband_keeps_setpoint(void)
{
    in.p_grid = -400;
    step();
    TEST_ASSERT_EQUAL_FLOAT(-400, out.batt_sp);
    in.p_batt = -400;
    in.p_grid = -20;
    step();
    TEST_ASSERT_EQUAL(RB_DEADBAND, out.batt_reason);
    TEST_ASSERT_EQUAL_FLOAT(-400, out.batt_sp);
}

/* Consigne tout juste écrite, mesures pas encore rafraîchies : rien ne bouge, sauf les sécurités. */
static void test_hold_keeps_setpoint_until_settled(void)
{
    in.p_grid = 500;
    step();
    TEST_ASSERT_EQUAL_FLOAT(500, out.batt_sp);
    in.batt_hold = true;                                /* p_batt encore à 0, p_grid encore à 500 */
    step();
    TEST_ASSERT_EQUAL(RB_SETTLE, out.batt_reason);
    TEST_ASSERT_EQUAL_FLOAT(500, out.batt_sp);          /* pas de double correction vers 1000 W */
    in.grid_ok = false;                                 /* le repli passe avant l'attente */
    step();
    TEST_ASSERT_EQUAL(RB_FALLBACK, out.batt_reason);
    TEST_ASSERT_EQUAL_FLOAT(0, out.batt_sp);
}

static void test_no_discharge_in_hc(void)
{
    in.hc = true;
    in.p_grid = 800;
    step();
    TEST_ASSERT_EQUAL_FLOAT(0, out.batt_sp);
    TEST_ASSERT_TRUE(out.batt_limits & RB_LIM_HC);
    in.p_grid = -400;                                   /* surplus solaire : charge permise */
    step();
    TEST_ASSERT_EQUAL_FLOAT(-400, out.batt_sp);
}

/* Arbitrage (§9.6) : en HC, charge depuis le réseau à la puissance du plan, avec la rampe, jusqu'au SOC prévu. */
static void test_arbitrage_charges_in_hc_only(void)
{
    in.hc = true;
    in.p_grid = 300;                    /* maison la nuit */
    in.arb_charge_w = 1000;
    in.arb_soc_target = 60;
    step();
    TEST_ASSERT_EQUAL_UINT8(RB_ARBITRAGE, out.batt_reason);
    TEST_ASSERT_EQUAL_FLOAT(-REG_RAMP_W, out.batt_sp);  /* rampe */
    for (int i = 0; i < 5; i++) {
        in.p_batt = out.batt_sp;
        in.p_grid = 300 - in.p_batt;                    /* la charge s'ajoute à l'import */
        step();
    }
    TEST_ASSERT_EQUAL_FLOAT(-1000, out.batt_sp);
    in.soc = 60;                                        /* cible atteinte : retour au zéro injection */
    step();
    TEST_ASSERT_EQUAL_UINT8(RB_TRACK, out.batt_reason);
    TEST_ASSERT_EQUAL_FLOAT(-400, out.batt_sp);         /* remonte vers 0 (pas de décharge en HC) */
    in.soc = 50;
    in.hc = false;                                      /* jamais en heures pleines */
    in.p_batt = 0;
    st.batt_sp = 0;
    step();
    TEST_ASSERT_NOT_EQUAL(RB_ARBITRAGE, out.batt_reason);
    TEST_ASSERT_TRUE(out.batt_sp >= 0);
}

static void test_arbitrage_respects_soc_max(void)
{
    in.hc = true;
    in.soc = 100;
    in.p_grid = 300;
    in.arb_charge_w = 1600;
    in.arb_soc_target = 100.5f;
    step();
    TEST_ASSERT_EQUAL_FLOAT(0, out.batt_sp);
    TEST_ASSERT_TRUE(out.batt_limits & RB_LIM_SOC_MAX);
}

static void test_soc_limits(void)
{
    in.soc = 10;
    in.p_grid = 600;
    step();
    TEST_ASSERT_EQUAL_FLOAT(0, out.batt_sp);
    TEST_ASSERT_TRUE(out.batt_limits & RB_LIM_SOC_MIN);
    in.soc = 100;
    in.p_grid = -1000;
    step();
    TEST_ASSERT_EQUAL_FLOAT(0, out.batt_sp);
    TEST_ASSERT_TRUE(out.batt_limits & RB_LIM_SOC_MAX);
}

static void test_shelly_invalid_rest_and_ce_off_outside_hc(void)
{
    in.override = REG_OVR_ON;                           /* forçage : déjà ON */
    in.override_until = in.now + H(1);
    run(60);
    TEST_ASSERT_TRUE(out.relay);
    in.grid_ok = false;
    in.p_grid = 900;
    step();
    TEST_ASSERT_EQUAL_FLOAT(0, out.batt_sp);
    TEST_ASSERT_EQUAL(RB_FALLBACK, out.batt_reason);
    TEST_ASSERT_FALSE(out.relay);                       /* sécurité : passe outre la durée min ON */
    TEST_ASSERT_EQUAL(RC_FALLBACK, out.ce_reason);
}

static void test_no_time_rest(void)
{
    in.time_ok = false;
    in.p_grid = -2000;
    run(300);
    TEST_ASSERT_EQUAL_FLOAT(0, out.batt_sp);
    TEST_ASSERT_FALSE(out.relay);
}

static void test_zendure_invalid_uses_real_surplus_only(void)
{
    in.batt_ok = false;
    in.p_batt = -1000;                                  /* ignoré */
    in.p_grid = -1200;                                  /* surplus réel 1200 < 1500 */
    run(300);
    TEST_ASSERT_EQUAL(RB_NO_BATT, out.batt_reason);
    TEST_ASSERT_FALSE(out.relay);
    in.p_grid = -1600;
    run(125);
    TEST_ASSERT_TRUE(out.relay);
}

/* ---------- Chauffe-eau : routage solaire ---------- */

static void test_sunny_solar_start_after_two_minutes(void)
{
    in.p_grid = -400;
    in.p_batt = -1200;                                  /* surplus disponible 1600 W */
    run(115);
    TEST_ASSERT_FALSE(out.relay);
    TEST_ASSERT_EQUAL(RC_SOLAR_WAIT, out.ce_reason);
    run(10);
    TEST_ASSERT_TRUE(out.relay);
}

static void test_solar_needs_soc_target(void)
{
    in.p_grid = -2000;
    in.soc = 30;                                        /* cible 40 % */
    run(300);
    TEST_ASSERT_FALSE(out.relay);
}

static void test_cloudy_never_starts(void)
{
    for (int i = 0; i < 120; i++) {                     /* surplus qui oscille sous le seuil */
        in.p_grid = (i % 2) ? -1700 : -900;
        run(30);
        TEST_ASSERT_FALSE(out.relay);
    }
}

static void test_winter_hc_only_no_day_heating(void)
{
    in.mode = CE_HC_ONLY;
    in.p_grid = -2500;
    run(600);
    TEST_ASSERT_FALSE(out.relay);
    TEST_ASSERT_EQUAL(RC_HC_ONLY, out.ce_reason);
}

static void test_min_on_and_min_off_respected(void)
{
    in.p_grid = -1600;
    run(125);
    TEST_ASSERT_TRUE(out.relay);
    int64_t on_at = st.relay_changed_at;
    in.p_grid = 1500;                                   /* plus de surplus */
    in.soc = 40;                                        /* batterie non autorisée */
    while (st.relay_cmd) {
        run(5);
    }
    TEST_ASSERT_TRUE(st.relay_changed_at - on_at >= 600);
    int64_t off_at = st.relay_changed_at;
    in.soc = 50;
    in.p_grid = -1600;                                  /* le surplus revient tout de suite */
    while (!st.relay_cmd) {
        run(5);
    }
    TEST_ASSERT_TRUE(st.relay_changed_at - off_at >= 300);
}

static void test_thermostat_cutoff_sets_tank_full(void)
{
    in.p_grid = -1600;
    run(125);
    TEST_ASSERT_TRUE(out.relay);
    in.p_ce = 2500;
    run(1200);
    in.p_ce = 40;                                       /* le thermostat coupe */
    bool event = false;
    for (int t = 0; t < 200 && !event; t += 5) {
        in.relay_on = st.relay_cmd;
        step();
        event = out.tank_full_event;
        in.now += 5;
    }
    TEST_ASSERT_TRUE(event);
    TEST_ASSERT_TRUE(st.wh.tank_full);
    run(60);
    TEST_ASSERT_FALSE(out.relay);
    TEST_ASSERT_EQUAL(RC_TANK_FULL, out.ce_reason);
    in.p_grid = -3000;                                  /* plus de chauffe solaire ce cycle */
    run(900);
    TEST_ASSERT_FALSE(out.relay);
}

static void test_override_on_and_off(void)
{
    in.mode = CE_HC_ONLY;
    in.override = REG_OVR_ON;
    in.override_until = in.now + 1800;
    step();
    TEST_ASSERT_TRUE(out.relay);
    TEST_ASSERT_EQUAL(RC_OVERRIDE_ON, out.ce_reason);
    run(1900);                                          /* fin du forçage */
    TEST_ASSERT_FALSE(out.relay);
    in.mode = CE_SOLAR_EARLY;
    in.p_grid = -2000;
    in.override = REG_OVR_OFF;
    in.override_until = in.now + 3600;
    run(600);
    TEST_ASSERT_FALSE(out.relay);
    TEST_ASSERT_EQUAL(RC_OVERRIDE_OFF, out.ce_reason);
}

static void test_new_cycle_at_shower_time(void)
{
    st.wh.tank_full = true;
    st.wh.e_heated_wh = 3000;
    in.last_shower = HM(20, 0);
    in.next_shower = HM(20, 0) + DAY;
    in.now = HM(20, 0) + 5;
    step();
    TEST_ASSERT_TRUE(out.new_cycle_event);
    TEST_ASSERT_FALSE(st.wh.tank_full);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, st.wh.e_heated_wh);
}

/* ---------- Chauffe-eau : stratégie du soir (§9.5) ---------- */

static const wh_window_t NIGHT = { HM(22, 0), HM(6, 0) + DAY };
#define SHOWER_NEXT (HM(20, 0) + DAY)

static void test_night_heat_ends_at_hc_end_never_at_start(void)
{
    wh_state_t s;
    wh_init(&s, &cfg.wh);
    wh_night_t n;
    TEST_ASSERT_TRUE(wh_night_plan(&cfg.wh, &s, &NIGHT, 1, SHOWER_NEXT, 0.0f, &n));
    TEST_ASSERT_TRUE(n.full);
    TEST_ASSERT_EQUAL_INT64(NIGHT.end, n.end);
    TEST_ASSERT_EQUAL_INT64(HM(4, 0) + DAY, n.start);   /* 06:00 − 105 min − 15 min */
    TEST_ASSERT_TRUE(n.start > NIGHT.start);
}

static void test_partial_preheat_when_sunny_tomorrow(void)
{
    wh_state_t s;
    wh_init(&s, &cfg.wh);
    float s_ce = wh_s_ce(&cfg.wh, 8000.0f, 2800.0f);    /* (8000 − 2800) × 0,7 = 3640 */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 3640.0f, s_ce);
    wh_night_t n;
    TEST_ASSERT_TRUE(wh_night_plan(&cfg.wh, &s, &NIGHT, 1, SHOWER_NEXT, s_ce, &n));
    TEST_ASSERT_FALSE(n.full);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, E_FULL - 3640.0f, n.e_target_wh);
    int64_t dur = (int64_t)lroundf((E_FULL - 3640.0f) / 2500.0f * 3600.0f);
    TEST_ASSERT_EQUAL_INT64(NIGHT.end - dur - 900, n.start);
}

static void test_no_night_heat_when_sun_covers_it(void)
{
    wh_state_t s;
    wh_init(&s, &cfg.wh);
    wh_night_t n;
    float s_ce = wh_s_ce(&cfg.wh, 12000.0f, 2800.0f);   /* 6440 ≥ E_full */
    TEST_ASSERT_FALSE(wh_night_plan(&cfg.wh, &s, &NIGHT, 1, SHOWER_NEXT, s_ce, &n));
    TEST_ASSERT_FALSE(n.active);
}

static void test_two_hc_windows_choose_closest_before_shower(void)
{
    wh_state_t s;
    wh_init(&s, &cfg.wh);
    const wh_window_t w[2] = { NIGHT, { HM(13, 0) + DAY, HM(15, 0) + DAY } };
    wh_night_t n;
    TEST_ASSERT_TRUE(wh_night_plan(&cfg.wh, &s, w, 2, SHOWER_NEXT, 0.0f, &n));
    TEST_ASSERT_EQUAL_INT64(HM(15, 0) + DAY, n.end);
    TEST_ASSERT_EQUAL_INT64(HM(13, 0) + DAY, n.start);  /* 120 min nécessaires : début de plage */
}

static void test_catchup_in_hp_at_the_right_time(void)
{
    /* 20:00 − T(E_full × marge de sécurité) − 15 min */
    const int64_t at = HM(20, 0) - (int64_t)lroundf(E_FULL * WH_CATCHUP_SAFETY / 2500.0f * 3600.0f) - 900;
    in.mode = CE_HC_ONLY;
    in.now = at - 60;
    run(55);
    TEST_ASSERT_FALSE(out.relay);
    TEST_ASSERT_EQUAL_INT64(at, out.catchup_at);
    run(10);
    TEST_ASSERT_TRUE(out.relay);
    TEST_ASSERT_EQUAL(RC_CATCHUP, out.ce_reason);
    /* Le reste diminue pendant la chauffe : le rattrapage ne doit pas s'interrompre. */
    for (int t = 0; t < 3600; t += 5) {
        run(5);
        TEST_ASSERT_TRUE(out.relay);
    }
}

static void test_battery_does_not_feed_heater_below_target(void)
{
    in.mode = CE_HC_ONLY;
    in.override = REG_OVR_ON;
    in.override_until = in.now + H(1);
    in.soc = 35;                                        /* cible 40 % */
    in.p_batt = 0;
    in.p_ce = 2500;
    in.p_grid = 2900;                                   /* 400 W de maison + 2500 W de chauffe-eau */
    in.relay_on = true;
    step();
    TEST_ASSERT_TRUE(out.relay);
    TEST_ASSERT_EQUAL_FLOAT(400, out.batt_sp);          /* 400 W de maison, pas 2900 W */
    in.p_batt = 400;
    in.p_grid = 2500;
    step();
    TEST_ASSERT_EQUAL_FLOAT(400, out.batt_sp);
    TEST_ASSERT_TRUE(out.batt_limits & RB_LIM_CE);
}

static void test_catchup_disabled_raises_alert(void)
{
    cfg.wh.catchup_hp = false;
    in.mode = CE_HC_ONLY;
    in.now = HM(18, 30);
    step();
    TEST_ASSERT_FALSE(out.relay);
    TEST_ASSERT_TRUE(out.catchup_blocked);
}

static void test_e_full_learned_on_cold_heat(void)
{
    wh_state_t s;
    wh_init(&s, &cfg.wh);
    s.run_from_cold = true;
    s.e_heated_wh = 5000.0f;
    wh_on_tank_full(&s, H(5));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.8f * E_FULL + 0.2f * 5000.0f, s.e_full_wh);
    TEST_ASSERT_TRUE(s.tank_full);
}

/* ---------- Garantie : un ballon plein par cycle, 30 jours de météo variée ---------- */

static uint32_t seed = 4242;
static float rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8) / 16777216.0f;
}

/* Puissance PV (W) à l'instant t pour une crête donnée, soleil de 08:00 à 18:00. */
static float pv_at(int64_t t, float peak)
{
    float h = (float)(t % DAY) / 3600.0f;
    return (h > 8.0f && h < 18.0f) ? peak * sinf((h - 8.0f) / 10.0f * 3.14159265f) : 0.0f;
}

static bool is_hc(int64_t t)
{
    int64_t s = t % DAY;
    return s >= H(22) || s < H(6);
}

static void plan_for(int64_t t0, float peak, float soc, plan_t *p)
{
    plan_in_t pi;
    memset(&pi, 0, sizeof(pi));
    pi.t0 = t0;
    pi.n = PLAN_SLOTS;
    pi.soc = soc;
    pi.soc_min = 10;
    pi.soc_max = 100;
    pi.capa_kwh = 3.2f;
    pi.eta = 0.88f;
    pi.wear_eur_kwh = 0.08f;
    pi.price_hp = 0.27f;
    pi.price_hc = 0.20f;
    pi.price_sell = 0.04f;
    pi.e_ce_wh = wh_remaining(&st.wh);
    int64_t day0 = t0 - t0 % DAY;
    pi.shower_at = day0 + HM(20, 0) + (t0 % DAY >= HM(20, 0) ? DAY : 0);
    for (int k = 0; k < PLAN_SLOTS; k++) {
        int64_t mid = t0 + (int64_t)k * 1800 + 900;
        pi.pv_wh[k] = pv_at(mid, peak) * 0.5f;
        pi.base_wh[k] = 400.0f * 0.5f;
        pi.hc[k] = is_hc(mid);
    }
    planner_compute(&pi, p);
}

static void test_tank_full_every_cycle_over_30_days(void)
{
    const int days = 30;
    float peaks[32];
    for (int d = 0; d < 32; d++) {
        float r = rnd();
        /* Hiver, nuageux, mitigé, ensoleillé. */
        peaks[d] = r < 0.25f ? 150.0f : r < 0.5f ? 700.0f : r < 0.75f ? 1600.0f : 2600.0f;
    }
    float deficit = 4000.0f;            /* Wh pour atteindre la coupure du thermostat */
    float soc = 30.0f;
    float p_batt = 0.0f;
    int64_t t = HM(20, 0) + 60;         /* jour 0, juste après les douches */
    in.now = t;
    in.dt_s = 60;
    in.last_shower = HM(20, 0);
    in.next_shower = HM(20, 0) + DAY;
    st.wh.cycle_start = HM(20, 0) - DAY;    /* le premier pas ouvre le cycle des douches de 20:00 */
    plan_t plan;
    plan_for(t, peaks[1], soc, &plan);
    wh_night_t night = { 0 };
    int failures = 0, cycles = 0, night_starts_at_hc_begin = 0;
    /* Diagnostic : REG_DIAG=1 (bilan par jour), REG_DIAG_DAY=N (trace 16:00–20:00 du jour N). */
    const bool diag = getenv("REG_DIAG") != NULL;
    const int diag_day = getenv("REG_DIAG_DAY") ? atoi(getenv("REG_DIAG_DAY")) : -1;
    const float THERMO_HYST_WH = 500.0f;
    bool thermo_cut = false;
    bool prev_relay = false;
    int on_minutes[RC_COUNT] = { 0 };
    int full_events = 0;

    for (; t <= HM(20, 0) + (int64_t)days * DAY; t += 60) {
        int64_t tod = t % DAY;
        int day = (int)(t / DAY);
        if (tod == HM(20, 0)) {
            cycles++;
            if (!st.wh.tank_full) {
                failures++;
            }
            deficit += 3000.0f + 2000.0f * rnd();          /* douches du soir */
            in.last_shower = t;
            in.next_shower = t + DAY;
        }
        if (tod == H(22)) {                                 /* plan de nuit avec la prévision du lendemain */
            float forecast = peaks[day + 1] * (0.8f + 0.4f * rnd());
            plan_for(t, forecast, soc, &plan);
            const wh_window_t w = { t, t + H(8) };
            wh_night_plan(&cfg.wh, &st.wh, &w, 1, in.next_shower,
                          wh_s_ce(&cfg.wh, plan.S_shower_wh, plan.B_wh), &night);
            if (night.active && night.start == w.start && !night.full) {
                night_starts_at_hc_begin++;
            }
        }
        if (tod == H(6)) {
            plan_for(t, peaks[day], soc, &plan);
        }

        /* Physique : ballon (pertes 40 Wh/h, thermostat avec hystérésis), batterie, réseau. */
        deficit += 40.0f / 60.0f;
        if (thermo_cut && deficit > THERMO_HYST_WH) {
            thermo_cut = false;                         /* l'eau a refroidi : le thermostat réarme */
        }
        float p_ce = 0.0f;
        if (st.relay_cmd && !thermo_cut) {
            p_ce = cfg.wh.power_w;
            deficit -= p_ce / 60.0f;
            if (deficit <= 0.0f) {
                deficit = 0.0f;
                thermo_cut = true;                      /* température de consigne atteinte */
            }
        }
        float pv = pv_at(t, peaks[day]);
        float base = (tod >= H(19) && tod < H(21)) ? 900.0f : 300.0f;
        soc -= p_batt / 60.0f / 3200.0f * 90.0f;
        soc = soc < 10.0f ? 10.0f : (soc > 100.0f ? 100.0f : soc);

        in.now = t;
        in.hc = is_hc(t);
        in.p_ce = p_ce;
        in.relay_on = st.relay_cmd;
        in.p_batt = p_batt;
        in.soc = soc;
        in.p_grid = base + p_ce - pv - p_batt;
        in.mode = plan.mode;
        in.soc_target = planner_soc_target(&plan, t, 10.0f);
        in.night = night;
        regulator_step(&in, &st, &out);
        if (diag && day == diag_day && tod >= H(16) && tod < H(20) && (out.relay != prev_relay || tod % 1800 == 0)) {
            printf("    %02d:%02d relais %d (%s) déficit %4.0f chauffé %4.0f rattrapage %02d:%02d soc %3.0f cible %3.0f\n",
                   (int)(tod / 3600), (int)(tod % 3600 / 60), out.relay, regulator_ce_reason_name(out.ce_reason),
                   (double)deficit, (double)st.wh.e_heated_wh, (int)(out.catchup_at % DAY / 3600),
                   (int)(out.catchup_at % 3600 / 60), (double)soc, (double)in.soc_target);
        }
        prev_relay = out.relay;
        if (diag) {
            if (out.relay) {
                on_minutes[out.ce_reason]++;
            }
            full_events += out.tank_full_event;
            if (tod == HM(19, 59)) {
                printf("  jour %2d : crête %4.0f W, mode %d, nuit %s, déficit %5.0f Wh, plein %d, E_full %4.0f\n",
                       day, (double)peaks[day], plan.mode, night.active ? "oui" : "non", (double)deficit,
                       st.wh.tank_full, (double)st.wh.e_full_wh);
            }
        }
        p_batt = out.batt_sp;
        if ((p_batt > 0 && soc <= 10.0f) || (p_batt < 0 && soc >= 100.0f)) {
            p_batt = 0.0f;
        }
    }
    if (diag) {
        printf("  ballon plein : %d événements ; minutes ON par raison :", full_events);
        for (int r = 0; r < RC_COUNT; r++) {
            if (on_minutes[r]) {
                printf(" %s=%d", regulator_ce_reason_name((uint8_t)r), on_minutes[r]);
            }
        }
        printf("\n");
    }
    TEST_ASSERT_EQUAL_INT(days, cycles);
    TEST_ASSERT_EQUAL_INT(0, failures);
    TEST_ASSERT_EQUAL_INT(0, night_starts_at_hc_begin);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zero_grid_tracking_with_ramp);
    RUN_TEST(test_deadband_keeps_setpoint);
    RUN_TEST(test_hold_keeps_setpoint_until_settled);
    RUN_TEST(test_no_discharge_in_hc);
    RUN_TEST(test_arbitrage_charges_in_hc_only);
    RUN_TEST(test_arbitrage_respects_soc_max);
    RUN_TEST(test_soc_limits);
    RUN_TEST(test_shelly_invalid_rest_and_ce_off_outside_hc);
    RUN_TEST(test_no_time_rest);
    RUN_TEST(test_zendure_invalid_uses_real_surplus_only);
    RUN_TEST(test_sunny_solar_start_after_two_minutes);
    RUN_TEST(test_solar_needs_soc_target);
    RUN_TEST(test_cloudy_never_starts);
    RUN_TEST(test_winter_hc_only_no_day_heating);
    RUN_TEST(test_min_on_and_min_off_respected);
    RUN_TEST(test_thermostat_cutoff_sets_tank_full);
    RUN_TEST(test_override_on_and_off);
    RUN_TEST(test_new_cycle_at_shower_time);
    RUN_TEST(test_night_heat_ends_at_hc_end_never_at_start);
    RUN_TEST(test_partial_preheat_when_sunny_tomorrow);
    RUN_TEST(test_no_night_heat_when_sun_covers_it);
    RUN_TEST(test_two_hc_windows_choose_closest_before_shower);
    RUN_TEST(test_catchup_in_hp_at_the_right_time);
    RUN_TEST(test_catchup_disabled_raises_alert);
    RUN_TEST(test_battery_does_not_feed_heater_below_target);
    RUN_TEST(test_e_full_learned_on_cold_heat);
    RUN_TEST(test_tank_full_every_cycle_over_30_days);
    return UNITY_END();
}
