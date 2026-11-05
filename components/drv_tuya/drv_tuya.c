#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "cJSON.h"
#include "tuya_proto.h"
#include "drv_tuya.h"

static const char *TAG = "drv_tuya";

#define TUYA_TIMEOUT_MS     3000
#define TUYA_ATTEMPTS       2           /* 1 essai + 1 nouvel essai, comme les autres pilotes */
#define TUYA_MAX_FRAMES     4           /* trames lues avant de trouver la réponse */
#define TUYA_JSON_MAX       1024

/* Tampons statiques : un seul appelant (task_poll). */
static uint8_t s_tx[512];
static uint8_t s_rx[TUYA_MAX_FRAME];
static uint8_t s_plain[TUYA_JSON_MAX + 16];
static uint8_t s_tmp[TUYA_MAX_FRAME];
static uint32_t s_seq;

/* ---------- Cryptographie (PSA, IDF v6) ---------- */

static int psa_aes_ecb(const uint8_t key[TUYA_KEY_LEN], bool decrypt, const uint8_t *in, size_t len, uint8_t *out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, decrypt ? PSA_KEY_USAGE_DECRYPT : PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECB_NO_PADDING);
    mbedtls_svc_key_id_t id;
    if (len > sizeof(s_tmp) || psa_import_key(&attr, key, TUYA_KEY_LEN, &id) != PSA_SUCCESS) {
        return -1;
    }
    size_t olen = 0;
    memcpy(s_tmp, in, len);     /* entrée et sortie peuvent se recouvrir */
    psa_status_t st = decrypt
        ? psa_cipher_decrypt(id, PSA_ALG_ECB_NO_PADDING, s_tmp, len, out, len, &olen)
        : psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, s_tmp, len, out, len, &olen);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS && olen == len) ? 0 : -1;
}

static int psa_hmac(const uint8_t key[TUYA_KEY_LEN], const uint8_t *data, size_t len, uint8_t out[TUYA_HMAC_LEN])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t id;
    if (psa_import_key(&attr, key, TUYA_KEY_LEN, &id) != PSA_SUCCESS) {
        return -1;
    }
    size_t olen = 0;
    psa_status_t st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data, len, out, TUYA_HMAC_LEN, &olen);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS && olen == TUYA_HMAC_LEN) ? 0 : -1;
}

static int psa_gcm(const uint8_t key[TUYA_KEY_LEN], bool decrypt, const uint8_t iv[TUYA_IV_LEN], const uint8_t *aad,
                   size_t aad_len, const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[TUYA_TAG_LEN])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, decrypt ? PSA_KEY_USAGE_DECRYPT : PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    mbedtls_svc_key_id_t id;
    if (len + TUYA_TAG_LEN > sizeof(s_tmp) || psa_import_key(&attr, key, TUYA_KEY_LEN, &id) != PSA_SUCCESS) {
        return -1;
    }
    size_t olen = 0;
    psa_status_t st;
    if (decrypt) {
        /* PSA attend chiffré ‖ étiquette. */
        memcpy(s_tmp, in, len);
        memcpy(s_tmp + len, tag, TUYA_TAG_LEN);
        st = psa_aead_decrypt(id, PSA_ALG_GCM, iv, TUYA_IV_LEN, aad, aad_len, s_tmp, len + TUYA_TAG_LEN,
                              out, len, &olen);
        st = (st == PSA_SUCCESS && olen == len) ? PSA_SUCCESS : PSA_ERROR_INVALID_SIGNATURE;
    } else {
        st = psa_aead_encrypt(id, PSA_ALG_GCM, iv, TUYA_IV_LEN, aad, aad_len, in, len, s_tmp,
                              len + TUYA_TAG_LEN, &olen);
        if (st == PSA_SUCCESS && olen == len + TUYA_TAG_LEN) {
            memcpy(out, s_tmp, len);
            memcpy(tag, s_tmp + len, TUYA_TAG_LEN);
        } else {
            st = PSA_ERROR_GENERIC_ERROR;
        }
    }
    psa_destroy_key(id);
    return st == PSA_SUCCESS ? 0 : -1;
}

static const tuya_crypto_t k_crypto = { psa_aes_ecb, psa_hmac, psa_gcm };

/* ---------- Socket ---------- */

static int tcp_connect(const char *ip)
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(TUYA_PORT) };
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return -1;
    }
    /* Connexion non bloquante bornée à TUYA_TIMEOUT_MS (la connexion bloquante de lwIP peut durer bien plus). */
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        struct timeval tv = { .tv_sec = TUYA_TIMEOUT_MS / 1000, .tv_usec = (TUYA_TIMEOUT_MS % 1000) * 1000 };
        int err = 0;
        socklen_t el = sizeof(err);
        if (select(s + 1, NULL, &wfds, NULL, &tv) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 &&
            err == 0) {
            r = 0;
        }
    }
    if (r != 0) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, fl);
    struct timeval tv = { .tv_sec = TUYA_TIMEOUT_MS / 1000, .tv_usec = (TUYA_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return s;
}

static bool recv_all(int s, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = recv(s, buf + got, len - got, 0);
        if (r <= 0) {
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

/* Lit une trame complète (55AA ou 6699) dans s_rx ; retourne sa longueur ou −1. */
static int recv_frame(int s)
{
    if (!recv_all(s, s_rx, 4)) {
        return -1;
    }
    size_t hdr = (s_rx[2] == 0x66 && s_rx[3] == 0x99) ? TUYA_HDR6699_LEN : TUYA_HDR_LEN;
    if (!recv_all(s, s_rx + 4, hdr - 4)) {
        return -1;
    }
    int total = tuya_frame_len(s_rx, hdr);
    if (total < 0 || !recv_all(s, s_rx + hdr, (size_t)total - hdr)) {
        return -1;
    }
    return total;
}

/* Lit et authentifie une trame : charge utile chiffrée (3.3/3.4) ou déjà déchiffrée (3.5). */
static int recv_parse(int s, tuya_ver_t v, const uint8_t *key, const uint8_t *hmac_key, tuya_frame_t *f)
{
    static uint8_t plain[TUYA_MAX_FRAME];
    int n = recv_frame(s);
    if (n < 0) {
        return -1;
    }
    if (v == TUYA_V35) {
        return tuya_parse_frame_6699(s_rx, (size_t)n, key, &k_crypto, f, plain, sizeof(plain));
    }
    return tuya_parse_frame(s_rx, (size_t)n, hmac_key, &k_crypto, f);
}

static esp_err_t send_cmd(int s, tuya_ver_t v, uint32_t cmd, const uint8_t *plain, size_t len,
                          const uint8_t *key, const uint8_t *hmac_key)
{
    static uint8_t enc[512];
    int n = tuya_encode_payload(v, cmd, plain, len, key, &k_crypto, enc, sizeof(enc));
    if (n < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (v == TUYA_V35) {
        uint8_t iv[TUYA_IV_LEN];
        esp_fill_random(iv, sizeof(iv));        /* IV aléatoire à chaque trame (tinytuya : os.urandom(12)) */
        n = tuya_build_frame_6699(++s_seq, cmd, enc, (size_t)n, key, iv, &k_crypto, s_tx, sizeof(s_tx));
    } else {
        n = tuya_build_frame(++s_seq, cmd, enc, (size_t)n, hmac_key, &k_crypto, s_tx, sizeof(s_tx));
    }
    if (n < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send(s, s_tx, (size_t)n, 0) == n ? ESP_OK : ESP_FAIL;
}

/* 3.4 / 3.5 : négociation de la clé de session (tinytuya _negotiate_session_key). */
static esp_err_t negotiate(int s, tuya_ver_t v, const uint8_t real_key[TUYA_KEY_LEN], uint8_t session[TUYA_KEY_LEN])
{
    uint8_t ln[TUYA_NONCE_LEN];
    esp_fill_random(ln, sizeof(ln));
    esp_err_t err = send_cmd(s, v, TUYA_CMD_SESS_START, ln, sizeof(ln), real_key, real_key);
    if (err != ESP_OK) {
        return err;
    }
    tuya_frame_t f;
    if (recv_parse(s, v, real_key, real_key, &f) != 0 || f.cmd != TUYA_CMD_SESS_RESP) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t *resp = f.payload;
    if (v == TUYA_V34) {
        /* 3.4 : réponse chiffrée en AES-ECB avec la clé locale. */
        if (f.len == 0 || f.len % 16 != 0 || f.len > sizeof(s_plain) ||
            psa_aes_ecb(real_key, true, f.payload, f.len, s_plain) != 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        resp = s_plain;
    }
    if (f.len < 48) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t rn[TUYA_NONCE_LEN];
    memcpy(rn, resp, sizeof(rn));
    uint8_t mac[TUYA_HMAC_LEN];
    if (psa_hmac(real_key, ln, sizeof(ln), mac) != 0 || memcmp(mac, resp + 16, TUYA_HMAC_LEN) != 0) {
        ESP_LOGW(TAG, "négociation : HMAC incorrect (clé locale ?)");
        return ESP_ERR_INVALID_CRC;
    }
    if (psa_hmac(real_key, rn, sizeof(rn), mac) != 0) {
        return ESP_FAIL;
    }
    err = send_cmd(s, v, TUYA_CMD_SESS_FINISH, mac, sizeof(mac), real_key, real_key);
    if (err != ESP_OK) {
        return err;
    }
    return tuya_session_key(v, real_key, ln, rn, &k_crypto, session) == 0 ? ESP_OK : ESP_FAIL;
}

static tuya_ver_t cfg_version(const cfg_t *cfg)
{
    return cfg->tuya.version == 35 ? TUYA_V35 : (cfg->tuya.version == 34 ? TUYA_V34 : TUYA_V33);
}

/* Ouvre la connexion et prépare la clé (session en 3.4). */
static int open_session(const cfg_t *cfg, tuya_ver_t v, uint8_t key[TUYA_KEY_LEN], esp_err_t *err)
{
    int s = tcp_connect(cfg->dev.tongou_ip);
    if (s < 0) {
        *err = ESP_ERR_TIMEOUT;
        return -1;
    }
    memcpy(key, cfg->tuya.local_key, TUYA_KEY_LEN);
    *err = ESP_OK;
    if (v != TUYA_V33) {
        uint8_t real[TUYA_KEY_LEN];
        memcpy(real, key, sizeof(real));
        *err = negotiate(s, v, real, key);
    }
    if (*err != ESP_OK) {
        close(s);
        return -1;
    }
    return s;
}

/* Extrait relais et puissance des DP. */
static esp_err_t parse_dps(const cfg_t *cfg, const char *json, tuya_reading_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *dps = cJSON_GetObjectItemCaseSensitive(root, "dps");
    if (!cJSON_IsObject(dps)) {
        /* 3.4 : {"data":{"dps":{...}}} */
        dps = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "data"), "dps");
    }
    char k_sw[4], k_pw[4];
    snprintf(k_sw, sizeof(k_sw), "%u", cfg->tuya.dp_switch);
    snprintf(k_pw, sizeof(k_pw), "%u", cfg->tuya.dp_power);
    const cJSON *sw = cJSON_GetObjectItemCaseSensitive(dps, k_sw);
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(dps, k_pw);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (cJSON_IsBool(sw) && cJSON_IsNumber(pw)) {
        out->relay_on = cJSON_IsTrue(sw);
        out->power_w = (float)pw->valuedouble / (float)cfg->tuya.power_div;
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "DP %s (relais) ou %s (puissance) absents de %.120s", k_sw, k_pw, json);
    }
    cJSON_Delete(root);
    return err;
}

/* Lit les trames jusqu'à la réponse à la requête, puis la déchiffre. */
static esp_err_t read_reply(int s, tuya_ver_t v, const uint8_t *key, const uint8_t *hmac_key)
{
    for (int i = 0; i < TUYA_MAX_FRAMES; i++) {
        tuya_frame_t f;
        if (recv_parse(s, v, key, hmac_key, &f) != 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (!f.auth_ok) {
            ESP_LOGW(TAG, "trame 0x%02lx : %s incorrect", (unsigned long)f.cmd, hmac_key ? "HMAC" : "CRC");
        }
        if (f.len == 0) {
            continue;       /* accusé de réception */
        }
        int m = tuya_decode_payload(v, f.payload, f.len, key, &k_crypto, s_plain, sizeof(s_plain));
        if (m < 0) {
            ESP_LOGW(TAG, "déchiffrement impossible (clé locale ou version ?)");
            return ESP_ERR_INVALID_CRC;
        }
        if (strstr((const char *)s_plain, "data unvalid") != NULL) {
            ESP_LOGW(TAG, "appareil « device22 » : variante non prise en charge");
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (strstr((const char *)s_plain, "\"dps\"") != NULL) {
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t poll_once(const cfg_t *cfg, tuya_reading_t *out)
{
    tuya_ver_t v = cfg_version(cfg);
    uint8_t key[TUYA_KEY_LEN];
    esp_err_t err;
    int s = open_session(cfg, v, key, &err);
    if (s < 0) {
        return err;
    }
    char json[160];
    uint32_t cmd = v == TUYA_V33 ? TUYA_CMD_DP_QUERY : TUYA_CMD_DP_QUERY_NEW;
    int n = tuya_query_json(v, cfg->tuya.dev_id, (uint32_t)time(NULL), json, sizeof(json));
    const uint8_t *hk = v == TUYA_V34 ? key : NULL;     /* HMAC en 3.4 ; 3.5 : étiquette GCM */
    err = n < 0 ? ESP_ERR_INVALID_SIZE : send_cmd(s, v, cmd, (const uint8_t *)json, (size_t)n, key, hk);
    if (err == ESP_OK) {
        err = read_reply(s, v, key, hk);
    }
    close(s);
    return err == ESP_OK ? parse_dps(cfg, (const char *)s_plain, out) : err;
}

static bool configured(const cfg_t *cfg)
{
    return cfg->tuya.dev_id[0] != '\0' && strlen(cfg->tuya.local_key) == TUYA_KEY_LEN;
}

/* Vérification des primitives PSA contre la référence Python (tools/tuya_ref.py, clé « 0123456789abcdef »,
 * nonces 00..0f et 10..1f) : clé de session AES-ECB et HMAC-SHA256 du nonce local. */
static bool crypto_selftest(void)
{
    static const uint8_t key[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    static const uint8_t ln[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t rn[16] = { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    static const uint8_t sk_ref[16] = {
        0x37, 0x72, 0x22, 0xe0, 0x61, 0xa9, 0x24, 0xc5, 0x91, 0xcd, 0x9c, 0x27, 0xea, 0x16, 0x3e, 0xd4,
    };
    static const uint8_t mac_ref[32] = {
        0xb0, 0x78, 0x5c, 0x28, 0x9e, 0x34, 0xbe, 0x92, 0xfa, 0x9f, 0x34, 0x0d, 0xaa, 0x07, 0xc0, 0xe0,
        0x83, 0x9c, 0x9b, 0xb8, 0xb2, 0xfd, 0xb1, 0xd4, 0xc1, 0x19, 0xd9, 0x46, 0x2e, 0x8a, 0xed, 0x4c,
    };
    /* 3.5 : clé de session AES-GCM (IV = nonce local[0:12]) et son étiquette. */
    static const uint8_t sk35_ref[16] = {
        0xed, 0x2f, 0xd4, 0x0d, 0x0e, 0x64, 0x2b, 0x26, 0xf8, 0x36, 0x17, 0x0a, 0xd0, 0x8b, 0xa0, 0x3d,
    };
    static const uint8_t tag35_ref[16] = {
        0x6e, 0x9a, 0xb7, 0xf2, 0xbc, 0x80, 0x74, 0x91, 0xaf, 0x95, 0x55, 0x34, 0x59, 0x23, 0x53, 0x9f,
    };
    uint8_t sk[16], mac[32], back[16], x[16], tag[16];
    bool ok = tuya_session_key(TUYA_V34, key, ln, rn, &k_crypto, sk) == 0 && memcmp(sk, sk_ref, 16) == 0 &&
              psa_aes_ecb(key, true, sk, 16, back) == 0 && back[0] == (ln[0] ^ rn[0]) &&
              psa_hmac(key, ln, 16, mac) == 0 && memcmp(mac, mac_ref, 32) == 0;
    for (int i = 0; i < 16; i++) {
        x[i] = ln[i] ^ rn[i];
    }
    bool gcm = psa_gcm(key, false, ln, NULL, 0, x, 16, sk, tag) == 0 && memcmp(sk, sk35_ref, 16) == 0 &&
               memcmp(tag, tag35_ref, 16) == 0 && psa_gcm(key, true, ln, NULL, 0, sk, 16, back, tag) == 0 &&
               memcmp(back, x, 16) == 0;
    tag[0] ^= 1;
    gcm = gcm && psa_gcm(key, true, ln, NULL, 0, sk, 16, back, tag) != 0;     /* étiquette fausse refusée */
    ESP_LOGI(TAG, "auto-test AES-ECB / HMAC-SHA256 : %s, AES-GCM : %s", ok ? "OK" : "ÉCHEC", gcm ? "OK" : "ÉCHEC");
    ok = ok && gcm;
    return ok;
}

static bool crypto_ready(void)
{
    static int state;       /* 0 inconnu, 1 prêt, −1 indisponible */
    if (state == 0) {
        state = (psa_crypto_init() == PSA_SUCCESS && crypto_selftest()) ? 1 : -1;
    }
    return state > 0;
}

void drv_tuya_selftest(void)
{
    crypto_ready();
}

esp_err_t drv_tuya_poll(const cfg_t *cfg, tuya_reading_t *out)
{
    if (!configured(cfg)) {
        ESP_LOGW(TAG, "identifiant ou clé locale Tuya non renseignés");
        return ESP_ERR_INVALID_STATE;
    }
    if (!crypto_ready()) {
        return ESP_FAIL;
    }
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < TUYA_ATTEMPTS && err != ESP_OK; i++) {
        err = poll_once(cfg, out);
        /* Clé ou DP faux : inutile de réessayer. */
        if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) {
            break;
        }
    }
    return err;
}

esp_err_t drv_tuya_set(const cfg_t *cfg, bool on)
{
    if (!configured(cfg) || !crypto_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    tuya_ver_t v = cfg_version(cfg);
    uint8_t key[TUYA_KEY_LEN];
    esp_err_t err;
    int s = open_session(cfg, v, key, &err);
    if (s < 0) {
        return err;
    }
    char json[200];
    int n = tuya_control_json(v, cfg->tuya.dev_id, (uint32_t)time(NULL), cfg->tuya.dp_switch, on, json, sizeof(json));
    uint32_t cmd = v == TUYA_V33 ? TUYA_CMD_CONTROL : TUYA_CMD_CONTROL_NEW;
    err = n < 0 ? ESP_ERR_INVALID_SIZE
                : send_cmd(s, v, cmd, (const uint8_t *)json, (size_t)n, key, v == TUYA_V34 ? key : NULL);
    close(s);
    return err;
}
