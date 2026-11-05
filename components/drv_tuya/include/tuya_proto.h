#pragma once

/* Protocole local Tuya 3.3 / 3.4, trames 55AA. C pur (testable sur PC), la cryptographie est injectée.
 * Référence : tinytuya (jasonacox/tinytuya, dossier core), relu le 2026-11-04 ; réimplémentation Python
 * équivalente dans tools/tuya_ref.py, qui génère les vecteurs de test/host/test_tuya.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUYA_PORT               6668
#define TUYA_HDR_LEN            16
#define TUYA_MAX_FRAME          (16 + 1440)   /* MAX_PAYLOAD_LENGTH de tinytuya */
#define TUYA_KEY_LEN            16
#define TUYA_NONCE_LEN          16
#define TUYA_HMAC_LEN           32

#define TUYA_CMD_SESS_START     0x03
#define TUYA_CMD_SESS_RESP      0x04
#define TUYA_CMD_SESS_FINISH    0x05
#define TUYA_CMD_CONTROL        0x07
#define TUYA_CMD_STATUS         0x08
#define TUYA_CMD_HEART_BEAT     0x09
#define TUYA_CMD_DP_QUERY       0x0A
#define TUYA_CMD_CONTROL_NEW    0x0D
#define TUYA_CMD_DP_QUERY_NEW   0x10

#define TUYA_HDR6699_LEN        18
#define TUYA_IV_LEN             12
#define TUYA_TAG_LEN            16

typedef enum {
    TUYA_V33 = 33,
    TUYA_V34 = 34,
    TUYA_V35 = 35,      /* trames 6699, AES-GCM (vérifié sur l'appareil de la maison le 2026-11-05) */
} tuya_ver_t;

typedef struct {
    /* AES-128-ECB sur len octets (multiple de 16). Retourne 0 si OK. */
    int (*aes_ecb)(const uint8_t key[TUYA_KEY_LEN], bool decrypt, const uint8_t *in, size_t len, uint8_t *out);
    /* HMAC-SHA256. Retourne 0 si OK. */
    int (*hmac_sha256)(const uint8_t key[TUYA_KEY_LEN], const uint8_t *data, size_t len, uint8_t out[TUYA_HMAC_LEN]);
    /* AES-128-GCM (3.5). Chiffrement : écrit tag ; déchiffrement : vérifie tag, −1 si incorrect. */
    int (*aes_gcm)(const uint8_t key[TUYA_KEY_LEN], bool decrypt, const uint8_t iv[TUYA_IV_LEN],
                   const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t len, uint8_t *out,
                   uint8_t tag[TUYA_TAG_LEN]);
} tuya_crypto_t;

typedef struct {
    uint32_t       seq;
    uint32_t       cmd;
    uint32_t       retcode;
    const uint8_t *payload;     /* dans le tampon de la trame */
    size_t         len;
    bool           auth_ok;     /* CRC (3.3) ou HMAC (3.4) correct */
} tuya_frame_t;

uint32_t tuya_crc32(const uint8_t *data, size_t len);

/* Charge utile chiffrée : 3.3 chiffre puis préfixe l'en-tête de version, 3.4 préfixe puis chiffre ;
 * pas d'en-tête pour les requêtes d'état et la négociation. Retourne la longueur ou −1. */
int tuya_encode_payload(tuya_ver_t v, uint32_t cmd, const uint8_t *plain, size_t len,
                        const uint8_t key[TUYA_KEY_LEN], const tuya_crypto_t *c, uint8_t *out, size_t cap);

/* Trame client : en-tête, charge utile, CRC32 (hmac_key NULL) ou HMAC-SHA256, suffixe. −1 si trop long. */
int tuya_build_frame(uint32_t seq, uint32_t cmd, const uint8_t *payload, size_t len,
                     const uint8_t *hmac_key, const tuya_crypto_t *c, uint8_t *out, size_t cap);

/* Longueur totale d'une trame d'après son en-tête (≥ 16 octets), −1 si invalide. */
int tuya_frame_len(const uint8_t *buf, size_t len);

/* Trame appareil (code retour avant la charge utile). −1 si malformée. */
int tuya_parse_frame(const uint8_t *buf, size_t len, const uint8_t *hmac_key, const tuya_crypto_t *c,
                     tuya_frame_t *f);

/* Déchiffre une charge utile d'appareil en JSON terminé par NUL. Retourne la longueur ou −1. */
int tuya_decode_payload(tuya_ver_t v, const uint8_t *payload, size_t len, const uint8_t key[TUYA_KEY_LEN],
                        const tuya_crypto_t *c, uint8_t *out, size_t cap);

/* JSON des requêtes, sans espaces (l'appareil ne répond pas sinon, d'après tinytuya). */
int tuya_query_json(tuya_ver_t v, const char *dev_id, uint32_t t, char *out, size_t cap);
int tuya_control_json(tuya_ver_t v, const char *dev_id, uint32_t t, unsigned dp, bool on, char *out, size_t cap);

/* Clé de session (nonce local XOR nonce distant, chiffré avec la clé locale) :
 * 3.4 : AES-ECB ; 3.5 : AES-GCM avec IV = 12 premiers octets du nonce local, sans AAD. */
int tuya_session_key(tuya_ver_t v, const uint8_t key[TUYA_KEY_LEN], const uint8_t local_nonce[TUYA_NONCE_LEN],
                     const uint8_t remote_nonce[TUYA_NONCE_LEN], const tuya_crypto_t *c,
                     uint8_t out[TUYA_KEY_LEN]);

/* ---------- 3.5 : trames 6699 ---------- */

/* En-tête de 18 octets, IV, GCM(raw), étiquette, suffixe ; AAD = en-tête sans le préfixe.
 * raw : charge utile en clair (en-tête de version compris, voir tuya_encode_payload). */
int tuya_build_frame_6699(uint32_t seq, uint32_t cmd, const uint8_t *raw, size_t len, const uint8_t key[TUYA_KEY_LEN],
                          const uint8_t iv[TUYA_IV_LEN], const tuya_crypto_t *c, uint8_t *out, size_t cap);

/* Trame appareil 6699 : déchiffre dans plain ; f->retcode = 4 premiers octets, f->payload pointe après.
 * −1 si malformée ou si l'étiquette GCM est incorrecte. */
int tuya_parse_frame_6699(const uint8_t *buf, size_t len, const uint8_t key[TUYA_KEY_LEN], const tuya_crypto_t *c,
                          tuya_frame_t *f, uint8_t *plain, size_t cap);
