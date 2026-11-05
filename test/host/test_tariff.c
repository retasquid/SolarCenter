#include "unity.h"
#include "tariff.h"

#define HM(h, m) ((uint16_t)((h) * 60 + (m)))

static tariff_t t;

void setUp(void)
{
    t = (tariff_t){
        .hphc = true, .n_hc = 1,
        .hc = { { HM(22, 0), HM(6, 0) } },
        .price_hp = 0.27f, .price_hc = 0.20f, .price_base = 0.25f, .price_sell = 0.04f,
    };
}

void tearDown(void)
{
}

static void test_slot_across_midnight(void)
{
    TEST_ASSERT_FALSE(tariff_is_hc(&t, HM(21, 59)));
    TEST_ASSERT_TRUE(tariff_is_hc(&t, HM(22, 0)));     /* début inclus */
    TEST_ASSERT_TRUE(tariff_is_hc(&t, HM(23, 59)));
    TEST_ASSERT_TRUE(tariff_is_hc(&t, HM(0, 0)));
    TEST_ASSERT_TRUE(tariff_is_hc(&t, HM(5, 59)));
    TEST_ASSERT_FALSE(tariff_is_hc(&t, HM(6, 0)));     /* fin exclue */
}

static const tariff_day_t TUE_JUN = { 2026, 6, 9, 2 };    /* mardi 9 juin 2026 */

static void test_prices(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.20f, tariff_price_at(&t, &TUE_JUN, HM(3, 0)));
    TEST_ASSERT_EQUAL_FLOAT(0.27f, tariff_price_at(&t, &TUE_JUN, HM(12, 0)));
    t.hphc = false;
    TEST_ASSERT_EQUAL_FLOAT(0.25f, tariff_price_at(&t, &TUE_JUN, HM(3, 0)));
    TEST_ASSERT_FALSE(tariff_is_hc(&t, HM(3, 0)));
}

/* Grille : saison basse/haute × semaine/week-end × HP/HC, plages HC 22:34–06:34 tous les jours. */
static void grid_setup(void)
{
    t.grid = true;
    t.hc[0].start_min = HM(22, 34);
    t.hc[0].end_min = HM(6, 34);
    t.high_months = (1u << 10) | (1u << 11) | (1u << 0) | (1u << 1) | (1u << 2);    /* nov.–mars */
    t.holidays_weekend = true;
    const float p[2][2][2] = {
        { { 0.1889f, 0.1595f }, { 0.1318f, 0.1256f } },     /* basse : semaine, week-end */
        { { 0.2530f, 0.1906f }, { 0.1959f, 0.1567f } },     /* haute */
    };
    for (int s = 0; s < 2; s++) {
        for (int d = 0; d < 2; d++) {
            for (int h = 0; h < 2; h++) {
                t.grid_price[s][d][h] = p[s][d][h];
            }
        }
    }
}

static void test_grid_season_weekday_weekend(void)
{
    grid_setup();
    const tariff_day_t sat_jun = { 2026, 6, 13, 6 };
    const tariff_day_t tue_jan = { 2027, 1, 12, 2 };
    const tariff_day_t sun_mar = { 2026, 3, 29, 0 };
    TEST_ASSERT_EQUAL_FLOAT(0.1889f, tariff_price_at(&t, &TUE_JUN, HM(12, 0)));
    TEST_ASSERT_EQUAL_FLOAT(0.1595f, tariff_price_at(&t, &TUE_JUN, HM(23, 0)));
    TEST_ASSERT_EQUAL_FLOAT(0.1889f, tariff_price_at(&t, &TUE_JUN, HM(22, 33)));    /* avant 22:34 */
    TEST_ASSERT_EQUAL_FLOAT(0.1318f, tariff_price_at(&t, &sat_jun, HM(12, 0)));
    TEST_ASSERT_EQUAL_FLOAT(0.1256f, tariff_price_at(&t, &sat_jun, HM(2, 0)));
    TEST_ASSERT_EQUAL_FLOAT(0.2530f, tariff_price_at(&t, &tue_jan, HM(18, 0)));
    TEST_ASSERT_EQUAL_FLOAT(0.1567f, tariff_price_at(&t, &sun_mar, HM(4, 0)));
    TEST_ASSERT_TRUE(tariff_high_season(&t, 11));
    TEST_ASSERT_FALSE(tariff_high_season(&t, 4));
}

static void test_grid_mean_price(void)
{
    grid_setup();
    /* 5 mois hauts / 12, 5 jours de semaine / 7. */
    float low = 5.0f / 7.0f * 0.1889f + 2.0f / 7.0f * 0.1318f;
    float high = 5.0f / 7.0f * 0.2530f + 2.0f / 7.0f * 0.1959f;
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 7.0f / 12.0f * low + 5.0f / 12.0f * high, tariff_mean_price(&t, false));
    t.grid = false;
    TEST_ASSERT_EQUAL_FLOAT(t.price_hc, tariff_mean_price(&t, true));
}

static void test_holidays(void)
{
    /* 2026 : Pâques le 5 avril → lundi 6 avril, Ascension 14 mai, lundi de Pentecôte 25 mai. */
    TEST_ASSERT_TRUE(tariff_is_holiday(2026, 4, 6));
    TEST_ASSERT_TRUE(tariff_is_holiday(2026, 5, 14));
    TEST_ASSERT_TRUE(tariff_is_holiday(2026, 5, 25));
    TEST_ASSERT_FALSE(tariff_is_holiday(2026, 4, 5));       /* Pâques est un dimanche */
    TEST_ASSERT_FALSE(tariff_is_holiday(2026, 5, 13));
    /* 2027 : Pâques le 28 mars → lundi 29 mars, Ascension 6 mai, Pentecôte lundi 17 mai. */
    TEST_ASSERT_TRUE(tariff_is_holiday(2027, 3, 29));
    TEST_ASSERT_TRUE(tariff_is_holiday(2027, 5, 6));
    TEST_ASSERT_TRUE(tariff_is_holiday(2027, 5, 17));
    TEST_ASSERT_TRUE(tariff_is_holiday(2026, 7, 14));
    TEST_ASSERT_TRUE(tariff_is_holiday(2026, 12, 25));

    grid_setup();
    const tariff_day_t mon_easter = { 2026, 4, 6, 1 };
    TEST_ASSERT_EQUAL_FLOAT(0.1318f, tariff_price_at(&t, &mon_easter, HM(12, 0)));   /* férié : week-end */
    t.holidays_weekend = false;
    TEST_ASSERT_EQUAL_FLOAT(0.1889f, tariff_price_at(&t, &mon_easter, HM(12, 0)));
}

static void test_slot_ending_at_midnight(void)
{
    t.hc[0].start_min = HM(22, 0);
    t.hc[0].end_min = 0;
    TEST_ASSERT_TRUE(tariff_is_hc(&t, HM(23, 59)));
    TEST_ASSERT_FALSE(tariff_is_hc(&t, HM(0, 0)));
}

static void test_two_slots_and_next_change(void)
{
    t.n_hc = 2;
    t.hc[0].start_min = HM(1, 0);
    t.hc[0].end_min = HM(7, 0);
    t.hc[1].start_min = HM(12, 30);
    t.hc[1].end_min = HM(14, 30);
    bool next_hc;
    TEST_ASSERT_EQUAL_UINT16(90, tariff_next_change(&t, HM(11, 0), &next_hc));
    TEST_ASSERT_TRUE(next_hc);
    TEST_ASSERT_EQUAL_UINT16(60, tariff_next_change(&t, HM(13, 30), &next_hc));
    TEST_ASSERT_FALSE(next_hc);
    /* Après la dernière plage : prochain changement le lendemain à 01:00. */
    TEST_ASSERT_EQUAL_UINT16(HM(1, 0) + 1440 - HM(20, 0), tariff_next_change(&t, HM(20, 0), &next_hc));
    TEST_ASSERT_TRUE(next_hc);
}

static void test_adjacent_slots_merge(void)
{
    t.n_hc = 2;
    t.hc[0].start_min = HM(22, 0);
    t.hc[0].end_min = 0;
    t.hc[1].start_min = 0;
    t.hc[1].end_min = HM(6, 0);
    bool next_hc;
    TEST_ASSERT_TRUE(tariff_is_hc(&t, HM(0, 0)));
    TEST_ASSERT_EQUAL_UINT16(HM(6, 0) + 1440 - HM(23, 0), tariff_next_change(&t, HM(23, 0), &next_hc));
    TEST_ASSERT_FALSE(next_hc);
}

static void test_base_never_changes(void)
{
    t.hphc = false;
    bool next_hc = true;
    TEST_ASSERT_EQUAL_UINT16(0, tariff_next_change(&t, HM(10, 0), &next_hc));
    TEST_ASSERT_FALSE(next_hc);
}

/* Changement d'heure : la minute locale est fournie par l'appelant (localtime). Le jour du
 * passage à l'heure d'été (02:00 → 03:00), les minutes 120–179 n'existent pas ; la plage
 * 22:00–06:00 reste correcte de part et d'autre du saut. */
static void test_dst_spring_forward_local_minutes(void)
{
    TEST_ASSERT_TRUE(tariff_is_hc(&t, HM(1, 59)));
    TEST_ASSERT_TRUE(tariff_is_hc(&t, HM(3, 0)));
    bool next_hc;
    TEST_ASSERT_EQUAL_UINT16(HM(3, 0), tariff_next_change(&t, HM(3, 0), &next_hc));
}

/* Passage à l'heure d'hiver : 02:00–02:59 se répète ; même minute locale, même tarif. */
static void test_dst_fall_back_repeated_hour(void)
{
    TEST_ASSERT_TRUE(tariff_is_hc(&t, HM(2, 30)));
    TEST_ASSERT_EQUAL_FLOAT(0.20f, tariff_price_at(&t, &TUE_JUN, HM(2, 30)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_slot_across_midnight);
    RUN_TEST(test_prices);
    RUN_TEST(test_slot_ending_at_midnight);
    RUN_TEST(test_two_slots_and_next_change);
    RUN_TEST(test_adjacent_slots_merge);
    RUN_TEST(test_base_never_changes);
    RUN_TEST(test_dst_spring_forward_local_minutes);
    RUN_TEST(test_dst_fall_back_repeated_hour);
    RUN_TEST(test_grid_season_weekday_weekend);
    RUN_TEST(test_holidays);
    RUN_TEST(test_grid_mean_price);
    return UNITY_END();
}
