#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"
#include "config.h"

static const char *TAG = "config";

#define NVS_NS  "cfg"
#define NVS_KEY "blob"

/* Blob versionné : le CRC32 porte sur cfg uniquement. */
typedef struct {
    uint16_t version;
    uint16_t size;
    uint32_t crc;
    cfg_t    cfg;
} cfg_blob_t;

static cfg_t s_cfg;
static cfg_blob_t s_blob;          /* tampon statique de lecture/écriture NVS */
static cfg_status_t s_status;

static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx;

#define BLOB_HDR_SIZE offsetof(cfg_blob_t, cfg)

_Static_assert(offsetof(cfg_t, shelly) == CFG_V1_SIZE, "les champs v2 doivent suivre cfg_t v1");
_Static_assert(offsetof(cfg_t, tuya) == CFG_V2_DATA, "les champs v3 doivent suivre cfg_t v2");

static uint32_t blob_crc(const cfg_blob_t *b, size_t size)
{
    return esp_rom_crc32_le(0, (const uint8_t *)&b->cfg, size);
}

/* Anciennes versions : taille de cfg_t enregistrée (CRC) et octets utiles à recopier. */
static const struct {
    uint16_t version;
    uint16_t size;
    uint16_t data;
} k_old_versions[] = {
    { 1, CFG_V1_SIZE, CFG_V1_SIZE },
    { 2, 1108, CFG_V2_DATA },           /* sizeof(cfg_t) v2 : 1106 octets utiles + 2 de remplissage */
};

/* Préfixe connu recopié, champs ajoutés à leurs valeurs par défaut. */
static cfg_status_t migrate(size_t len)
{
    for (size_t i = 0; i < sizeof(k_old_versions) / sizeof(k_old_versions[0]); i++) {
        if (s_blob.version != k_old_versions[i].version) {
            continue;
        }
        uint16_t size = k_old_versions[i].size;
        if (s_blob.size != size || len != BLOB_HDR_SIZE + size) {
            return CFG_STATUS_RESET_VERSION;
        }
        if (blob_crc(&s_blob, size) != s_blob.crc) {
            return CFG_STATUS_RESET_CRC;
        }
        static cfg_t tmp;
        cfg_set_defaults(&tmp);
        memcpy(&tmp, &s_blob.cfg, k_old_versions[i].data);
        const char *bad = cfg_validate(&tmp);
        if (bad != NULL) {
            ESP_LOGE(TAG, "config v%u invalide après migration (%s)", s_blob.version, bad);
            return CFG_STATUS_RESET_INVALID;
        }
        memcpy(&s_cfg, &tmp, sizeof(s_cfg));
        ESP_LOGW(TAG, "config migrée de v%u vers v%d", s_blob.version, CFG_VERSION);
        return CFG_STATUS_MIGRATED;
    }
    return CFG_STATUS_RESET_VERSION;
}

static cfg_status_t load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return CFG_STATUS_FIRST_BOOT;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open : %s", esp_err_to_name(err));
        return CFG_STATUS_RESET_READ;
    }

    size_t len = sizeof(s_blob);
    err = nvs_get_blob(h, NVS_KEY, &s_blob, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return CFG_STATUS_FIRST_BOOT;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        /* Blob plus grand que la structure actuelle : autre version. */
        return CFG_STATUS_RESET_VERSION;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob : %s", esp_err_to_name(err));
        return CFG_STATUS_RESET_READ;
    }
    if (len >= BLOB_HDR_SIZE && s_blob.version < CFG_VERSION) {
        return migrate(len);
    }
    if (len != sizeof(s_blob) || s_blob.version != CFG_VERSION || s_blob.size != sizeof(cfg_t)) {
        return CFG_STATUS_RESET_VERSION;
    }
    if (blob_crc(&s_blob, sizeof(s_blob.cfg)) != s_blob.crc) {
        return CFG_STATUS_RESET_CRC;
    }
    const char *bad = cfg_validate(&s_blob.cfg);
    if (bad != NULL) {
        ESP_LOGE(TAG, "config enregistrée invalide (%s)", bad);
        return CFG_STATUS_RESET_INVALID;
    }
    memcpy(&s_cfg, &s_blob.cfg, sizeof(s_cfg));
    return CFG_STATUS_OK;
}

esp_err_t config_init(void)
{
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    if (s_mtx == NULL) {
        return ESP_FAIL;
    }
    cfg_set_defaults(&s_cfg);
    s_status = load();
    if (s_status == CFG_STATUS_MIGRATED) {
        /* Réécrit au nouveau format ; le statut reste « migrée » pour l'interface. */
        static cfg_t migrated;
        memcpy(&migrated, &s_cfg, sizeof(migrated));
        if (config_save(&migrated, NULL) == ESP_OK) {
            s_status = CFG_STATUS_MIGRATED;
        }
    } else if (s_status == CFG_STATUS_OK) {
        ESP_LOGI(TAG, "config v%d chargée (%u octets)", CFG_VERSION, (unsigned)sizeof(cfg_t));
    } else {
        ESP_LOGW(TAG, "valeurs par défaut utilisées : %s", config_status_str(s_status));
    }
    return ESP_OK;
}

void config_get(cfg_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(out, &s_cfg, sizeof(*out));
    xSemaphoreGive(s_mtx);
}

esp_err_t config_save(const cfg_t *in, const char **bad_field)
{
    const char *bad = cfg_validate(in);
    if (bad_field != NULL) {
        *bad_field = bad;
    }
    if (bad != NULL) {
        ESP_LOGW(TAG, "config refusée : %s", bad);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_blob.version = CFG_VERSION;
    s_blob.size = sizeof(cfg_t);
    memcpy(&s_blob.cfg, in, sizeof(s_blob.cfg));
    s_blob.crc = blob_crc(&s_blob, sizeof(s_blob.cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &s_blob, sizeof(s_blob));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        memcpy(&s_cfg, in, sizeof(s_cfg));
        s_status = CFG_STATUS_OK;
    }
    xSemaphoreGive(s_mtx);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "écriture NVS impossible : %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "config enregistrée");
    }
    return err;
}

cfg_status_t config_status(void)
{
    return s_status;
}

const char *config_status_str(cfg_status_t st)
{
    switch (st) {
    case CFG_STATUS_OK:            return "ok";
    case CFG_STATUS_FIRST_BOOT:    return "first_boot";
    case CFG_STATUS_RESET_VERSION: return "reset_version";
    case CFG_STATUS_RESET_CRC:     return "reset_crc";
    case CFG_STATUS_RESET_INVALID: return "reset_invalid";
    case CFG_STATUS_RESET_READ:    return "reset_read";
    case CFG_STATUS_MIGRATED:      return "migrated";
    }
    return "unknown";
}

void config_tariff(const cfg_t *c, tariff_t *out)
{
    memset(out, 0, sizeof(*out));
    out->hphc = c->tariff.type == TARIFF_HPHC;
    out->n_hc = c->tariff.n_hc;
    out->price_hp = c->tariff.price_hp;
    out->price_hc = c->tariff.price_hc;
    out->price_base = c->tariff.price_base;
    out->price_sell = c->tariff.price_sell;
    for (int i = 0; i < CFG_HC_SLOTS_MAX && i < TARIFF_SLOTS_MAX; i++) {
        out->hc[i].start_min = c->tariff.hc[i].start_min;
        out->hc[i].end_min = c->tariff.hc[i].end_min;
    }
}

bool config_admin_is_set(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool set = s_cfg.sys.admin_pass[0] != '\0';
    xSemaphoreGive(s_mtx);
    return set;
}

bool config_check_admin(const char *pass)
{
    char given[CFG_ADMIN_PASS_LEN] = { 0 };
    size_t len = strnlen(pass, sizeof(given));
    if (len >= sizeof(given)) {
        return false;
    }
    memcpy(given, pass, len);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    /* Les octets après le NUL stocké sont traités comme des zéros. */
    size_t slen = strnlen(s_cfg.sys.admin_pass, sizeof(s_cfg.sys.admin_pass));
    uint8_t diff = 0;
    for (size_t i = 0; i < sizeof(given); i++) {
        char stored = (i < slen) ? s_cfg.sys.admin_pass[i] : '\0';
        diff |= (uint8_t)(given[i] ^ stored);
    }
    bool set = s_cfg.sys.admin_pass[0] != '\0';
    xSemaphoreGive(s_mtx);
    return set && diff == 0;
}
