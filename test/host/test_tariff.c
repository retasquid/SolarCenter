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

static void test_prices(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.20f, tariff_price(&t, HM(3, 0)));
    TEST_ASSERT_EQUAL_FLOAT(0.27f, tariff_price(&t, HM(12, 0)));
    t.hphc = false;
    TEST_ASSERT_EQUAL_FLOAT(0.25f, tariff_price(&t, HM(3, 0)));
    TEST_ASSERT_FALSE(tariff_is_hc(&t, HM(3, 0)));
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
    TEST_ASSERT_EQUAL_FLOAT(0.20f, tariff_price(&t, HM(2, 30)));
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
    return UNITY_END();
}
