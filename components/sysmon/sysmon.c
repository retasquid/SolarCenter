#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "app_state.h"
#include "net.h"
#include "sysmon.h"

static const char *TAG = "sysmon";

#define SYSMON_PERIOD_MS        5000
#define SYSMON_LOG_PERIOD_MS    60000
#define HEAP_WARN_BYTES         (40 * 1024)
#define HEAP_REBOOT_BYTES       (25 * 1024)
#define HEAP_REBOOT_OTA_BYTES   (12 * 1024)
#define EXPECTED_FLASH_BYTES    (4 * 1024 * 1024)
#define CHIP_REV_SECURE_BOOT_V2 300     /* v3.0 */

#define SYSMON_STACK            3072
#define SYSMON_PRIO             1
#define SYSMON_CORE             0

#define RESTART_DELAY_MS        1500    /* laisse partir la réponse HTTP */
#define OTA_VALID_AFTER_US      (120LL * 1000000)

static sysmon_safe_hook_t s_safe_hook;
static StaticTask_t s_task_buf;
static StackType_t s_task_stack[SYSMON_STACK];
static TaskHandle_t s_task;
static const char *volatile s_restart_reason;
static bool s_ota_checked;
static volatile bool s_ota_active;

void sysmon_set_ota_active(bool active)
{
    s_ota_active = active;
}

/* Nouvelle image OTA : validée après 2 min sans plantage, sinon le chargeur revient à l'ancienne. */
static void ota_check_valid(void)
{
    if (s_ota_checked || esp_timer_get_time() < OTA_VALID_AFTER_US) {
        return;
    }
    s_ota_checked = true;
    esp_ota_img_states_t state;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == NULL || esp_ota_get_state_partition(run, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "nouveau firmware validé (%s)", run->label);
    } else {
        ESP_LOGE(TAG, "validation du firmware impossible : %s", esp_err_to_name(err));
    }
}

void sysmon_log_boot_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s v%s (ESP-IDF %s)", app->project_name, app->version, app->idf_ver);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "ESP32 révision v%d.%d, %d cœurs", chip.revision / 100, chip.revision % 100, chip.cores);
    if (chip.revision < CHIP_REV_SECURE_BOOT_V2) {
        ESP_LOGW(TAG, "révision < v3.0 : pas de Secure Boot v2, OTA local protégé par mot de passe uniquement");
    }

    uint32_t flash = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "taille de flash illisible : %s", esp_err_to_name(err));
    } else if (flash != EXPECTED_FLASH_BYTES) {
        ESP_LOGW(TAG, "flash de %lu Ko, la table de partitions suppose 4096 Ko", (unsigned long)(flash / 1024));
    } else {
        ESP_LOGI(TAG, "flash 4096 Ko");
    }

    ESP_LOGI(TAG, "cause du dernier redémarrage : %d", (int)esp_reset_reason());
}

static void sysmon_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    TickType_t last_log = xTaskGetTickCount() - pdMS_TO_TICKS(SYSMON_LOG_PERIOD_MS);

    for (;;) {
        uint32_t free_b = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        uint32_t min_b = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
        int8_t rssi = net_rssi();

        app_state_t *st = app_state_lock();
        st->heap_free = free_b;
        st->heap_min = min_b;
        st->rssi = rssi;
        app_state_unlock();

        /* Pendant une OTA, c'est l'OTA qui abandonne (28 Ko) : ne pas couper l'écriture en plein milieu. */
        if (free_b < (s_ota_active ? HEAP_REBOOT_OTA_BYTES : HEAP_REBOOT_BYTES)) {
            ESP_LOGE(TAG, "heap critique : %lu o libres", (unsigned long)free_b);
            sysmon_restart("heap critique");
        }

        TickType_t now = xTaskGetTickCount();
        if (now - last_log >= pdMS_TO_TICKS(SYSMON_LOG_PERIOD_MS)) {
            last_log = now;
            if (free_b < HEAP_WARN_BYTES || min_b < HEAP_WARN_BYTES) {
                ESP_LOGW(TAG, "heap bas : %lu o libres, minimum %lu o",
                         (unsigned long)free_b, (unsigned long)min_b);
            } else {
                ESP_LOGI(TAG, "heap %lu o libres, minimum %lu o, RSSI %d dBm",
                         (unsigned long)free_b, (unsigned long)min_b, rssi);
            }
        }

        ota_check_valid();
        esp_task_wdt_reset();
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYSMON_PERIOD_MS)) != 0 && s_restart_reason != NULL) {
            vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
            sysmon_restart(s_restart_reason);
        }
    }
}

esp_err_t sysmon_start(void)
{
    s_task = xTaskCreateStaticPinnedToCore(sysmon_task, "task_sys", SYSMON_STACK, NULL,
                                           SYSMON_PRIO, s_task_stack, &s_task_buf, SYSMON_CORE);
    return s_task != NULL ? ESP_OK : ESP_FAIL;
}

void sysmon_request_restart(const char *reason)
{
    s_restart_reason = reason;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    } else {
        sysmon_restart(reason);
    }
}

void sysmon_set_safe_hook(sysmon_safe_hook_t hook)
{
    s_safe_hook = hook;
}

void sysmon_restart(const char *reason)
{
    ESP_LOGW(TAG, "redémarrage contrôlé : %s", reason);
    if (s_safe_hook != NULL) {
        s_safe_hook();
    }
    esp_restart();
}
