#include <math.h>
#include <stddef.h>
#include <string.h>
#include "unity.h"
#include "cfg_types.h"

static cfg_t c;

void setUp(void)
{
    cfg_set_defaults(&c);
}

void tearDown(void)
{
}

static cfg_slot_t slot(int h1, int m1, int h2, int m2)
{
    cfg_slot_t s = { (uint16_t)(h1 * 60 + m1), (uint16_t)(h2 * 60 + m2) };
    return s;
}

static void test_defaults_are_valid(void)
{
    TEST_ASSERT_NULL(cfg_validate(&c));
    TEST_ASSERT_FALSE(c.tariff.configured);
    TEST_ASSERT_TRUE(c.sys.observer);
}

static void test_hphc_configured_needs_a_slot(void)
{
    c.tariff.configured = true;
    c.tariff.type = TARIFF_HPHC;
    TEST_ASSERT_EQUAL_STRING("tariff.hc", cfg_validate(&c));
    c.tariff.type = TARIFF_BASE;
    TEST_ASSERT_NULL(cfg_validate(&c));
}

static void test_slot_across_midnight_is_valid(void)
{
    c.tariff.configured = true;
    c.tariff.n_hc = 2;
    c.tariff.hc[0] = slot(22, 0, 6, 0);
    c.tariff.hc[1] = slot(12, 30, 14, 30);
    TEST_ASSERT_NULL(cfg_validate(&c));
}

static void test_slot_ending_at_midnight_is_valid(void)
{
    c.tariff.n_hc = 2;
    c.tariff.hc[0] = slot(22, 0, 0, 0);
    c.tariff.hc[1] = slot(0, 0, 6, 0);
    TEST_ASSERT_NULL(cfg_validate(&c));
}

static void test_overlapping_slots_rejected(void)
{
    c.tariff.n_hc = 2;
    c.tariff.hc[0] = slot(1, 0, 7, 0);
    c.tariff.hc[1] = slot(6, 0, 8, 0);
    TEST_ASSERT_EQUAL_STRING("tariff.hc", cfg_validate(&c));
}

static void test_overlap_across_midnight_rejected(void)
{
    c.tariff.n_hc = 2;
    c.tariff.hc[0] = slot(22, 0, 6, 0);
    c.tariff.hc[1] = slot(5, 0, 7, 0);
    TEST_ASSERT_EQUAL_STRING("tariff.hc", cfg_validate(&c));
}

static void test_empty_or_out_of_day_slot_rejected(void)
{
    c.tariff.n_hc = 1;
    c.tariff.hc[0] = slot(3, 0, 3, 0);
    TEST_ASSERT_EQUAL_STRING("tariff.hc", cfg_validate(&c));
    c.tariff.hc[0].start_min = CFG_MINUTES_PER_DAY;
    TEST_ASSERT_EQUAL_STRING("tariff.hc", cfg_validate(&c));
    c.tariff.n_hc = CFG_HC_SLOTS_MAX + 1;
    TEST_ASSERT_EQUAL_STRING("tariff.hc", cfg_validate(&c));
}

static void test_price_bounds_and_nan(void)
{
    c.tariff.price_hp = 2.01f;
    TEST_ASSERT_EQUAL_STRING("tariff.price_hp", cfg_validate(&c));
    c.tariff.price_hp = NAN;
    TEST_ASSERT_EQUAL_STRING("tariff.price_hp", cfg_validate(&c));
    c.tariff.price_hp = 0.27f;
    c.tariff.price_sell = 1.5f;
    TEST_ASSERT_EQUAL_STRING("tariff.price_sell", cfg_validate(&c));
}

static void test_pv_bounds(void)
{
    c.pv.kwc = 0.05f;
    TEST_ASSERT_EQUAL_STRING("pv.kwc", cfg_validate(&c));
    c.pv.kwc = 36.0f;
    TEST_ASSERT_NULL(cfg_validate(&c));
    c.pv.tilt = 91.0f;
    TEST_ASSERT_EQUAL_STRING("pv.tilt", cfg_validate(&c));
}

static void test_soc_min_below_soc_max(void)
{
    c.batt.soc_min = 90;
    c.batt.soc_max = 90;
    TEST_ASSERT_EQUAL_STRING("batt.soc_min", cfg_validate(&c));
    c.batt.soc_max = 101;
    TEST_ASSERT_EQUAL_STRING("batt.soc_max", cfg_validate(&c));
}

static void test_commissioning_date(void)
{
    c.batt.commissioning = 20260229;    /* 2026 n'est pas bissextile */
    TEST_ASSERT_EQUAL_STRING("batt.commissioning", cfg_validate(&c));
    c.batt.commissioning = 20280229;
    TEST_ASSERT_NULL(cfg_validate(&c));
    c.batt.commissioning = 20261301;
    TEST_ASSERT_EQUAL_STRING("batt.commissioning", cfg_validate(&c));
}

static void test_month_fractions_sum_to_100(void)
{
    c.roi.month_frac[0] += 1.0f;
    TEST_ASSERT_EQUAL_STRING("roi.month_frac", cfg_validate(&c));
}

static void test_ipv4(void)
{
    TEST_ASSERT_TRUE(cfg_is_ipv4("192.168.1.20"));
    TEST_ASSERT_TRUE(cfg_is_ipv4("0.0.0.0"));
    TEST_ASSERT_FALSE(cfg_is_ipv4("192.168.1.256"));
    TEST_ASSERT_FALSE(cfg_is_ipv4("192.168.1"));
    TEST_ASSERT_FALSE(cfg_is_ipv4("192.168.1.1.1"));
    TEST_ASSERT_FALSE(cfg_is_ipv4("192.168..1"));
    TEST_ASSERT_FALSE(cfg_is_ipv4("1234.1.1.1"));
    TEST_ASSERT_FALSE(cfg_is_ipv4(" 1.1.1.1"));

    strcpy(c.dev.shelly_ip, "192.168.1.300");
    TEST_ASSERT_EQUAL_STRING("dev.shelly_ip", cfg_validate(&c));
    c.dev.shelly_ip[0] = '\0';
    TEST_ASSERT_NULL(cfg_validate(&c));
}

static void test_unterminated_string_rejected(void)
{
    memset(c.dev.zendure_sn, 'A', sizeof(c.dev.zendure_sn));
    TEST_ASSERT_EQUAL_STRING("dev.zendure_sn", cfg_validate(&c));
}

static void test_token_charset(void)
{
    strcpy(c.dev.envoy_token, "eyJhbGciOi.eyJleHAiOjE3.sig-_x");
    TEST_ASSERT_NULL(cfg_validate(&c));
    strcpy(c.dev.envoy_token, "Bearer eyJ");
    TEST_ASSERT_EQUAL_STRING("dev.envoy_token", cfg_validate(&c));
}

static void test_passwords(void)
{
    strcpy(c.sys.admin_pass, "court");
    TEST_ASSERT_EQUAL_STRING("sys.admin_pass", cfg_validate(&c));
    strcpy(c.sys.admin_pass, "assez long");
    TEST_ASSERT_NULL(cfg_validate(&c));
    strcpy(c.sys.wifi_pass, "1234567");
    TEST_ASSERT_EQUAL_STRING("sys.wifi_pass", cfg_validate(&c));
}

/* Migration v1 → v2 : les champs v2 commencent exactement à la fin de cfg_t v1. */
static void test_v2_fields_follow_v1_layout(void)
{
    TEST_ASSERT_EQUAL_UINT(CFG_V1_SIZE, offsetof(cfg_t, shelly));
    TEST_ASSERT_EQUAL_UINT(CFG_V2_DATA, offsetof(cfg_t, tuya));
}

static void test_tuya_fields(void)
{
    TEST_ASSERT_EQUAL_UINT8(CE_DEV_OPENBEKEN, c.tuya.type);
    c.tuya.type = CE_DEV_TUYA;
    strcpy(c.tuya.dev_id, "bf1234567890abcdefgh");
    strcpy(c.tuya.local_key, "a'b^c`d|e~f[g]h$");      /* caractères spéciaux courants */
    TEST_ASSERT_NULL(cfg_validate(&c));
    strcpy(c.tuya.local_key, "trop-courte");
    TEST_ASSERT_EQUAL_STRING("dev.tuya_key", cfg_validate(&c));
    c.tuya.local_key[0] = '\0';
    c.tuya.version = 36;
    TEST_ASSERT_EQUAL_STRING("dev.tuya_version", cfg_validate(&c));
    c.tuya.version = 35;
    strcpy(c.tuya.dev_id, "bf12-34");
    TEST_ASSERT_EQUAL_STRING("dev.tuya_id", cfg_validate(&c));
}

static void test_shelly_mode_and_channel(void)
{
    TEST_ASSERT_EQUAL_UINT8(SHELLY_MODE_AUTO, c.shelly.mode);
    c.shelly.mode = 3;
    TEST_ASSERT_EQUAL_STRING("dev.shelly_mode", cfg_validate(&c));
    c.shelly.mode = SHELLY_MODE_MONOPHASE;
    c.shelly.channel = 3;
    TEST_ASSERT_EQUAL_STRING("dev.shelly_channel", cfg_validate(&c));
    c.shelly.channel = 2;
    TEST_ASSERT_NULL(cfg_validate(&c));
}

static void test_hostname(void)
{
    strcpy(c.sys.hostname, "Solaire");
    TEST_ASSERT_EQUAL_STRING("sys.hostname", cfg_validate(&c));
    strcpy(c.sys.hostname, "-solaire");
    TEST_ASSERT_EQUAL_STRING("sys.hostname", cfg_validate(&c));
    c.sys.hostname[0] = '\0';
    TEST_ASSERT_EQUAL_STRING("sys.hostname", cfg_validate(&c));
    strcpy(c.sys.hostname, "solaire-2");
    TEST_ASSERT_NULL(cfg_validate(&c));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_valid);
    RUN_TEST(test_hphc_configured_needs_a_slot);
    RUN_TEST(test_slot_across_midnight_is_valid);
    RUN_TEST(test_slot_ending_at_midnight_is_valid);
    RUN_TEST(test_overlapping_slots_rejected);
    RUN_TEST(test_overlap_across_midnight_rejected);
    RUN_TEST(test_empty_or_out_of_day_slot_rejected);
    RUN_TEST(test_price_bounds_and_nan);
    RUN_TEST(test_pv_bounds);
    RUN_TEST(test_soc_min_below_soc_max);
    RUN_TEST(test_commissioning_date);
    RUN_TEST(test_month_fractions_sum_to_100);
    RUN_TEST(test_ipv4);
    RUN_TEST(test_unterminated_string_rejected);
    RUN_TEST(test_token_charset);
    RUN_TEST(test_passwords);
    RUN_TEST(test_hostname);
    RUN_TEST(test_v2_fields_follow_v1_layout);
    RUN_TEST(test_shelly_mode_and_channel);
    RUN_TEST(test_tuya_fields);
    return UNITY_END();
}
