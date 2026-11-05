#include <stdio.h>
#include <string.h>
#include "tuya_proto.h"

#define PREFIX          0x000055AAu
#define SUFFIX          0x0000AA55u
#define PREFIX_6699     0x00006699u
#define SUFFIX_6699     0x00009966u
#define VER_HDR_LEN     15          /* "3.x" + 12 octets nuls */
#define CRC_TRAILER     8           /* CRC32 + suffixe */
#define HMAC_TRAILER    (TUYA_HMAC_LEN + 4)

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* CRC-32 de zlib (binascii.crc32), polynôme réfléchi 0xEDB88320. */
uint32_t tuya_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static bool no_version_header(uint32_t cmd)
{
    switch (cmd) {
    case TUYA_CMD_DP_QUERY:
    case TUYA_CMD_DP_QUERY_NEW:
    case 0x12:                      /* UPDATEDPS */
    case TUYA_CMD_HEART_BEAT:
    case TUYA_CMD_SESS_START:
    case TUYA_CMD_SESS_RESP:
    case TUYA_CMD_SESS_FINISH:
    case 0x40:                      /* LAN_EXT_STREAM */
        return true;
    default:
        return false;
    }
}

static void version_header(tuya_ver_t v, uint8_t *out)
{
    memset(out, 0, VER_HDR_LEN);
    out[0] = '3';
    out[1] = '.';
    out[2] = (char)('0' + (int)v % 10);
}

static bool has_version_header(tuya_ver_t v, const uint8_t *p, size_t len)
{
    uint8_t h[VER_HDR_LEN];
    version_header(v, h);
    return len >= VER_HDR_LEN && memcmp(p, h, 3) == 0;
}

/* PKCS#7 puis AES-ECB de [in, in+len[ vers out. Retourne la longueur chiffrée ou −1. */
static int pad_encrypt(const uint8_t *in, size_t len, const uint8_t key[TUYA_KEY_LEN],
                       const tuya_crypto_t *c, uint8_t *out, size_t cap)
{
    size_t padded = (len / 16 + 1) * 16;
    if (padded > cap) {
        return -1;
    }
    if (out != in) {
        memmove(out, in, len);
    }
    memset(out + len, (int)(padded - len), padded - len);
    return c->aes_ecb(key, false, out, padded, out) == 0 ? (int)padded : -1;
}

int tuya_encode_payload(tuya_ver_t v, uint32_t cmd, const uint8_t *plain, size_t len,
                        const uint8_t key[TUYA_KEY_LEN], const tuya_crypto_t *c, uint8_t *out, size_t cap)
{
    bool hdr = !no_version_header(cmd);
    if (v == TUYA_V35) {
        /* En clair : le chiffrement GCM se fait à la construction de la trame. */
        size_t off = hdr ? VER_HDR_LEN : 0;
        if (off + len > cap) {
            return -1;
        }
        memmove(out + off, plain, len);
        if (hdr) {
            version_header(v, out);
        }
        return (int)(off + len);
    }
    if (v == TUYA_V34) {
        /* En-tête dans le chiffré. */
        size_t off = hdr ? VER_HDR_LEN : 0;
        if (off + len > cap) {
            return -1;
        }
        memmove(out + off, plain, len);
        if (hdr) {
            version_header(v, out);
        }
        return pad_encrypt(out, off + len, key, c, out, cap);
    }
    /* 3.3 : en-tête en clair devant le chiffré. */
    size_t off = hdr ? VER_HDR_LEN : 0;
    if (off > cap) {
        return -1;
    }
    int n = pad_encrypt(plain, len, key, c, out + off, cap - off);
    if (n < 0) {
        return -1;
    }
    if (hdr) {
        version_header(v, out);
    }
    return n + (int)off;
}

int tuya_build_frame(uint32_t seq, uint32_t cmd, const uint8_t *payload, size_t len,
                     const uint8_t *hmac_key, const tuya_crypto_t *c, uint8_t *out, size_t cap)
{
    size_t trailer = hmac_key ? HMAC_TRAILER : CRC_TRAILER;
    size_t total = TUYA_HDR_LEN + len + trailer;
    if (total > cap || total > TUYA_MAX_FRAME) {
        return -1;
    }
    put_be32(out, PREFIX);
    put_be32(out + 4, seq);
    put_be32(out + 8, cmd);
    put_be32(out + 12, (uint32_t)(len + trailer));      /* tout ce qui suit l'en-tête */
    memmove(out + TUYA_HDR_LEN, payload, len);
    size_t data_len = TUYA_HDR_LEN + len;
    if (hmac_key) {
        if (c->hmac_sha256(hmac_key, out, data_len, out + data_len) != 0) {
            return -1;
        }
        put_be32(out + data_len + TUYA_HMAC_LEN, SUFFIX);
    } else {
        put_be32(out + data_len, tuya_crc32(out, data_len));
        put_be32(out + data_len + 4, SUFFIX);
    }
    return (int)total;
}

int tuya_frame_len(const uint8_t *buf, size_t len)
{
    if (len >= TUYA_HDR6699_LEN && get_be32(buf) == PREFIX_6699) {
        uint32_t l = get_be32(buf + 14);
        if (l > TUYA_MAX_FRAME - TUYA_HDR6699_LEN - 4) {
            return -1;
        }
        return (int)(TUYA_HDR6699_LEN + l + 4);     /* + suffixe */
    }
    if (len < TUYA_HDR_LEN || get_be32(buf) != PREFIX) {
        return -1;
    }
    uint32_t l = get_be32(buf + 12);
    if (l > TUYA_MAX_FRAME - TUYA_HDR_LEN) {
        return -1;
    }
    return (int)(TUYA_HDR_LEN + l);
}

int tuya_build_frame_6699(uint32_t seq, uint32_t cmd, const uint8_t *raw, size_t len, const uint8_t key[TUYA_KEY_LEN],
                          const uint8_t iv[TUYA_IV_LEN], const tuya_crypto_t *c, uint8_t *out, size_t cap)
{
    size_t total = TUYA_HDR6699_LEN + TUYA_IV_LEN + len + TUYA_TAG_LEN + 4;
    if (total > cap || total > TUYA_MAX_FRAME) {
        return -1;
    }
    put_be32(out, PREFIX_6699);
    out[4] = 0;
    out[5] = 0;
    put_be32(out + 6, seq);
    put_be32(out + 10, cmd);
    put_be32(out + 14, (uint32_t)(len + TUYA_IV_LEN + TUYA_TAG_LEN));
    uint8_t *p = out + TUYA_HDR6699_LEN;
    memcpy(p, iv, TUYA_IV_LEN);
    memmove(p + TUYA_IV_LEN, raw, len);
    if (c->aes_gcm(key, false, iv, out + 4, TUYA_HDR6699_LEN - 4, p + TUYA_IV_LEN, len, p + TUYA_IV_LEN,
                   p + TUYA_IV_LEN + len) != 0) {
        return -1;
    }
    put_be32(out + total - 4, SUFFIX_6699);
    return (int)total;
}

int tuya_parse_frame_6699(const uint8_t *buf, size_t len, const uint8_t key[TUYA_KEY_LEN], const tuya_crypto_t *c,
                          tuya_frame_t *f, uint8_t *plain, size_t cap)
{
    int total = tuya_frame_len(buf, len);
    if (total < 0 || get_be32(buf) != PREFIX_6699 || (size_t)total > len ||
        (size_t)total < TUYA_HDR6699_LEN + TUYA_IV_LEN + 4 + TUYA_TAG_LEN + 4 ||
        get_be32(buf + total - 4) != SUFFIX_6699) {
        return -1;
    }
    const uint8_t *iv = buf + TUYA_HDR6699_LEN;
    const uint8_t *ct = iv + TUYA_IV_LEN;
    size_t ct_len = (size_t)total - TUYA_HDR6699_LEN - TUYA_IV_LEN - TUYA_TAG_LEN - 4;
    uint8_t tag[TUYA_TAG_LEN];
    memcpy(tag, ct + ct_len, TUYA_TAG_LEN);
    if (ct_len + 1 > cap || c->aes_gcm(key, true, iv, buf + 4, TUYA_HDR6699_LEN - 4, ct, ct_len, plain, tag) != 0) {
        return -1;
    }
    f->seq = get_be32(buf + 6);
    f->cmd = get_be32(buf + 10);
    f->retcode = get_be32(plain);
    f->payload = plain + 4;
    f->len = ct_len - 4;
    f->auth_ok = true;
    return 0;
}

int tuya_parse_frame(const uint8_t *buf, size_t len, const uint8_t *hmac_key, const tuya_crypto_t *c,
                     tuya_frame_t *f)
{
    int total = tuya_frame_len(buf, len);
    size_t trailer = hmac_key ? HMAC_TRAILER : CRC_TRAILER;
    if (total < 0 || (size_t)total > len || (size_t)total < TUYA_HDR_LEN + 4 + trailer) {
        return -1;
    }
    size_t data_len = (size_t)total - trailer;
    if (get_be32(buf + data_len + trailer - 4) != SUFFIX) {
        return -1;
    }
    f->seq = get_be32(buf + 4);
    f->cmd = get_be32(buf + 8);
    f->retcode = get_be32(buf + TUYA_HDR_LEN);
    f->payload = buf + TUYA_HDR_LEN + 4;
    f->len = data_len - TUYA_HDR_LEN - 4;
    if (hmac_key) {
        uint8_t mac[TUYA_HMAC_LEN];
        f->auth_ok = c->hmac_sha256(hmac_key, buf, data_len, mac) == 0 &&
                     memcmp(mac, buf + data_len, TUYA_HMAC_LEN) == 0;
    } else {
        f->auth_ok = tuya_crc32(buf, data_len) == get_be32(buf + data_len);
    }
    return 0;
}

static int decrypt_unpad(const uint8_t *in, size_t len, const uint8_t key[TUYA_KEY_LEN],
                         const tuya_crypto_t *c, uint8_t *out, size_t cap)
{
    if (len == 0 || len % 16 != 0 || len > cap) {
        return -1;
    }
    if (c->aes_ecb(key, true, in, len, out) != 0) {
        return -1;
    }
    uint8_t n = out[len - 1];
    if (n < 1 || n > 16) {
        return -1;
    }
    return (int)(len - n);
}

int tuya_decode_payload(tuya_ver_t v, const uint8_t *payload, size_t len, const uint8_t key[TUYA_KEY_LEN],
                        const tuya_crypto_t *c, uint8_t *out, size_t cap)
{
    if (cap < 1) {
        return -1;
    }
    int n;
    if (v == TUYA_V35) {
        /* Déjà déchiffrée par tuya_parse_frame_6699. */
        if (has_version_header(v, payload, len)) {
            payload += VER_HDR_LEN;
            len -= VER_HDR_LEN;
        }
        if (len + 1 > cap) {
            return -1;
        }
        memmove(out, payload, len);
        n = (int)len;
    } else if (v == TUYA_V34) {
        n = decrypt_unpad(payload, len, key, c, out, cap - 1);
        if (n >= VER_HDR_LEN && has_version_header(v, out, (size_t)n)) {
            memmove(out, out + VER_HDR_LEN, (size_t)n - VER_HDR_LEN);
            n -= VER_HDR_LEN;
        }
    } else {
        if (has_version_header(v, payload, len)) {
            payload += VER_HDR_LEN;
            len -= VER_HDR_LEN;
        }
        n = decrypt_unpad(payload, len, key, c, out, cap - 1);
    }
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    return n;
}

int tuya_query_json(tuya_ver_t v, const char *dev_id, uint32_t t, char *out, size_t cap)
{
    int n = v >= TUYA_V34
        ? snprintf(out, cap, "{}")
        : snprintf(out, cap, "{\"gwId\":\"%s\",\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lu\"}",
                   dev_id, dev_id, dev_id, (unsigned long)t);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int tuya_control_json(tuya_ver_t v, const char *dev_id, uint32_t t, unsigned dp, bool on, char *out, size_t cap)
{
    const char *val = on ? "true" : "false";
    int n = v >= TUYA_V34
        ? snprintf(out, cap, "{\"protocol\":5,\"t\":%lu,\"data\":{\"dps\":{\"%u\":%s}}}", (unsigned long)t, dp, val)
        : snprintf(out, cap, "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lu\",\"dps\":{\"%u\":%s}}",
                   dev_id, dev_id, (unsigned long)t, dp, val);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

int tuya_session_key(tuya_ver_t v, const uint8_t key[TUYA_KEY_LEN], const uint8_t local_nonce[TUYA_NONCE_LEN],
                     const uint8_t remote_nonce[TUYA_NONCE_LEN], const tuya_crypto_t *c,
                     uint8_t out[TUYA_KEY_LEN])
{
    uint8_t x[TUYA_KEY_LEN];
    for (int i = 0; i < TUYA_KEY_LEN; i++) {
        x[i] = local_nonce[i] ^ remote_nonce[i];
    }
    if (v == TUYA_V35) {
        uint8_t tag[TUYA_TAG_LEN];      /* non utilisée : la clé est le chiffré seul */
        return c->aes_gcm(key, false, local_nonce, NULL, 0, x, TUYA_KEY_LEN, out, tag);
    }
    return c->aes_ecb(key, false, x, TUYA_KEY_LEN, out);
}
