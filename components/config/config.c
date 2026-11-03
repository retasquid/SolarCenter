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

static uint32_t blob_crc(const cfg_blob_t *b)
{
    return esp_rom_crc32_le(0, (const uint8_t *)&b->cfg, sizeof(b->cfg));
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
    /* Version 1 : aucune migration possible pour l'instant. */
    if (len != sizeof(s_blob) || s_blob.version != CFG_VERSION || s_blob.size != sizeof(cfg_t)) {
        return CFG_STATUS_RESET_VERSION;
    }
    if (blob_crc(&s_blob) != s_blob.crc) {
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
    if (s_status == CFG_STATUS_OK) {
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
    s_blob.crc = blob_crc(&s_blob);

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
    }
    return "unknown";
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
