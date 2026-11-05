#include <string.h>
#include "unity.h"
#include "tuya_proto.h"
#include "tuya_vectors.h"

/* Cryptographie de test : chaque appel doit figurer dans la table enregistrée par la référence Python
 * (tools/tuya_ref.py vectors). Toute différence d'octets, d'ordre ou de clé échoue. */
static int lookup(uint8_t op, const uint8_t *key, const uint8_t *in, size_t len, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < sizeof(tv_calls) / sizeof(tv_calls[0]); i++) {
        const tv_call_t *c = &tv_calls[i];
        if (c->op == op && c->in_len == len && c->out_len == out_len && memcmp(c->key, key, 16) == 0 &&
            memcmp(c->in, in, len) == 0) {
            memcpy(out, c->out, out_len);
            return 0;
        }
    }
    return -1;
}

static int stub_aes(const uint8_t key[16], bool decrypt, const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t tmp[TUYA_MAX_FRAME];
    memcpy(tmp, in, len);
    return lookup(decrypt ? 1 : 0, key, tmp, len, out, len);
}

static int stub_hmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t out[32])
{
    return lookup(2, key, data, len, out, 32);
}

/* Table GCM : entrée = IV ‖ AAD ‖ données (‖ étiquette en déchiffrement), sortie = chiffré ‖ étiquette
 * (chiffrement) ou clair (déchiffrement). */
static int stub_gcm(const uint8_t key[16], bool decrypt, const uint8_t iv[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16])
{
    uint8_t q[TUYA_MAX_FRAME + 64];
    uint8_t r[TUYA_MAX_FRAME + 16];
    size_t n = 0;
    memcpy(q + n, iv, 12);
    n += 12;
    if (aad_len) {
        memcpy(q + n, aad, aad_len);
        n += aad_len;
    }
    memcpy(q + n, in, len);
    n += len;
    if (decrypt) {
        memcpy(q + n, tag, 16);
        n += 16;
        if (lookup(4, key, q, n, r, len) != 0) {
            return -1;
        }
        memcpy(out, r, len);
        return 0;
    }
    if (lookup(3, key, q, n, r, len + 16) != 0) {
        return -1;
    }
    memcpy(out, r, len);
    memcpy(tag, r + len, 16);
    return 0;
}

static const tuya_crypto_t C = { stub_aes, stub_hmac, stub_gcm };
static const uint8_t *KEY = (const uint8_t *)TV_KEY;
static uint8_t buf[TUYA_MAX_FRAME];
static uint8_t frame[TUYA_MAX_FRAME];

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_crc32_matches_zlib(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926, tuya_crc32((const uint8_t *)"123456789", 9));
}

static int build(tuya_ver_t v, uint32_t seq, uint32_t cmd, const char *json, const uint8_t *key, bool hmac)
{
    int n = tuya_encode_payload(v, cmd, (const uint8_t *)json, strlen(json), key, &C, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    return tuya_build_frame(seq, cmd, buf, (size_t)n, hmac ? key : NULL, &C, frame, sizeof(frame));
}

static void test_v33_query_frame(void)
{
    char json[160];
    TEST_ASSERT_TRUE(tuya_query_json(TUYA_V33, TV_DEV, TV_T, json, sizeof(json)) > 0);
    int n = build(TUYA_V33, 1, TUYA_CMD_DP_QUERY, json, KEY, false);
    TEST_ASSERT_EQUAL_INT(sizeof(tv_q33), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_q33, frame, n);
}

static void test_v33_control_frame(void)
{
    char json[160];
    TEST_ASSERT_TRUE(tuya_control_json(TUYA_V33, TV_DEV, TV_T, 1, true, json, sizeof(json)) > 0);
    int n = build(TUYA_V33, 2, TUYA_CMD_CONTROL, json, KEY, false);
    TEST_ASSERT_EQUAL_INT(sizeof(tv_c33), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_c33, frame, n);
}

static void test_v33_parse_device_reply(void)
{
    tuya_frame_t f;
    TEST_ASSERT_EQUAL_INT(sizeof(tv_r33), tuya_frame_len(tv_r33, sizeof(tv_r33)));
    TEST_ASSERT_EQUAL_INT(0, tuya_parse_frame(tv_r33, sizeof(tv_r33), NULL, &C, &f));
    TEST_ASSERT_TRUE(f.auth_ok);
    TEST_ASSERT_EQUAL_HEX32(TUYA_CMD_DP_QUERY, f.cmd);
    TEST_ASSERT_EQUAL_UINT32(0, f.retcode);
    uint8_t out[256];
    int n = tuya_decode_payload(TUYA_V33, f.payload, f.len, KEY, &C, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT((int)strlen(tv_r33_json), n);
    TEST_ASSERT_EQUAL_STRING(tv_r33_json, (const char *)out);
}

static void test_v33_bad_crc_detected(void)
{
    uint8_t copy[sizeof(tv_r33)];
    memcpy(copy, tv_r33, sizeof(copy));
    copy[20] ^= 0x01;
    tuya_frame_t f;
    TEST_ASSERT_EQUAL_INT(0, tuya_parse_frame(copy, sizeof(copy), NULL, &C, &f));
    TEST_ASSERT_FALSE(f.auth_ok);
}

static void test_v34_session_key(void)
{
    uint8_t sk[16];
    TEST_ASSERT_EQUAL_INT(0, tuya_session_key(TUYA_V34, KEY, tv_lnonce, tv_rnonce, &C, sk));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_skey, sk, 16);
}

static void test_v34_negotiation_start_frame(void)
{
    int n = tuya_encode_payload(TUYA_V34, TUYA_CMD_SESS_START, tv_lnonce, 16, KEY, &C, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(32, n);
    n = tuya_build_frame(1, TUYA_CMD_SESS_START, buf, (size_t)n, KEY, &C, frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT(sizeof(tv_neg_start), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_neg_start, frame, n);
}

static void test_v34_query_and_control_frames(void)
{
    char json[160];
    TEST_ASSERT_TRUE(tuya_query_json(TUYA_V34, TV_DEV, TV_T, json, sizeof(json)) > 0);
    int n = build(TUYA_V34, 3, TUYA_CMD_DP_QUERY_NEW, json, tv_skey, true);
    TEST_ASSERT_EQUAL_INT(sizeof(tv_q34), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_q34, frame, n);

    TEST_ASSERT_TRUE(tuya_control_json(TUYA_V34, TV_DEV, TV_T, 1, false, json, sizeof(json)) > 0);
    n = build(TUYA_V34, 4, TUYA_CMD_CONTROL_NEW, json, tv_skey, true);
    TEST_ASSERT_EQUAL_INT(sizeof(tv_c34), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_c34, frame, n);
}

static void test_v34_parse_device_reply(void)
{
    tuya_frame_t f;
    TEST_ASSERT_EQUAL_INT(0, tuya_parse_frame(tv_r34, sizeof(tv_r34), tv_skey, &C, &f));
    TEST_ASSERT_TRUE(f.auth_ok);
    TEST_ASSERT_EQUAL_HEX32(TUYA_CMD_DP_QUERY_NEW, f.cmd);
    uint8_t out[256];
    int n = tuya_decode_payload(TUYA_V34, f.payload, f.len, tv_skey, &C, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(tv_r34_json, (const char *)out);
}

/* ---------- 3.5 ---------- */

static void test_v35_session_key(void)
{
    uint8_t sk[16];
    TEST_ASSERT_EQUAL_INT(0, tuya_session_key(TUYA_V35, KEY, tv_lnonce, tv_rnonce, &C, sk));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_skey35, sk, 16);
}

static void test_v35_negotiation_frames(void)
{
    int n = tuya_encode_payload(TUYA_V35, TUYA_CMD_SESS_START, tv_lnonce, 16, KEY, &C, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(16, n);                       /* pas d'en-tête de version, pas de remplissage */
    n = tuya_build_frame_6699(1, TUYA_CMD_SESS_START, buf, (size_t)n, KEY, tv_iv_a, &C, frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT(sizeof(tv_neg35), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_neg35, frame, n);

    /* Réponse de l'appareil : code retour, nonce distant, HMAC du nonce local. */
    tuya_frame_t f;
    static uint8_t plain[256];
    TEST_ASSERT_EQUAL_INT(sizeof(tv_resp35), tuya_frame_len(tv_resp35, sizeof(tv_resp35)));
    TEST_ASSERT_EQUAL_INT(0, tuya_parse_frame_6699(tv_resp35, sizeof(tv_resp35), KEY, &C, &f, plain, sizeof(plain)));
    TEST_ASSERT_EQUAL_HEX32(TUYA_CMD_SESS_RESP, f.cmd);
    TEST_ASSERT_EQUAL_UINT32(0, f.retcode);
    TEST_ASSERT_EQUAL_INT(48, f.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_rnonce, f.payload, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_hmac_ln, f.payload + 16, 32);
}

static void test_v35_query_and_reply(void)
{
    char json[16];
    TEST_ASSERT_TRUE(tuya_query_json(TUYA_V35, TV_DEV, TV_T, json, sizeof(json)) > 0);
    int n = tuya_encode_payload(TUYA_V35, TUYA_CMD_DP_QUERY_NEW, (const uint8_t *)json, strlen(json), tv_skey35, &C,
                                buf, sizeof(buf));
    n = tuya_build_frame_6699(2, TUYA_CMD_DP_QUERY_NEW, buf, (size_t)n, tv_skey35, tv_iv_c, &C, frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT(sizeof(tv_q35), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tv_q35, frame, n);

    tuya_frame_t f;
    static uint8_t plain[256];
    TEST_ASSERT_EQUAL_INT(0, tuya_parse_frame_6699(tv_r35, sizeof(tv_r35), tv_skey35, &C, &f, plain, sizeof(plain)));
    uint8_t out[256];
    n = tuya_decode_payload(TUYA_V35, f.payload, f.len, tv_skey35, &C, out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING(tv_r35_json, (const char *)out);
}

static void test_v35_bad_tag_rejected(void)
{
    uint8_t copy[sizeof(tv_r35)];
    memcpy(copy, tv_r35, sizeof(copy));
    copy[sizeof(copy) - 6] ^= 0x01;                     /* étiquette GCM altérée */
    tuya_frame_t f;
    static uint8_t plain[256];
    TEST_ASSERT_EQUAL_INT(-1, tuya_parse_frame_6699(copy, sizeof(copy), tv_skey35, &C, &f, plain, sizeof(plain)));
}

static void test_rejects_malformed(void)
{
    tuya_frame_t f;
    uint8_t junk[32] = { 0x12, 0x34 };
    TEST_ASSERT_EQUAL_INT(-1, tuya_frame_len(junk, sizeof(junk)));
    TEST_ASSERT_EQUAL_INT(-1, tuya_parse_frame(tv_r33, sizeof(tv_r33) - 1, NULL, &C, &f));
    TEST_ASSERT_EQUAL_INT(-1, tuya_parse_frame(junk, sizeof(junk), NULL, &C, &f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc32_matches_zlib);
    RUN_TEST(test_v33_query_frame);
    RUN_TEST(test_v33_control_frame);
    RUN_TEST(test_v33_parse_device_reply);
    RUN_TEST(test_v33_bad_crc_detected);
    RUN_TEST(test_v34_session_key);
    RUN_TEST(test_v34_negotiation_start_frame);
    RUN_TEST(test_v34_query_and_control_frames);
    RUN_TEST(test_v34_parse_device_reply);
    RUN_TEST(test_v35_session_key);
    RUN_TEST(test_v35_negotiation_frames);
    RUN_TEST(test_v35_query_and_reply);
    RUN_TEST(test_v35_bad_tag_rejected);
    RUN_TEST(test_rejects_malformed);
    return UNITY_END();
}
