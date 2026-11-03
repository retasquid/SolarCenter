#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "app_state.h"
#include "net.h"
#include "sysmon.h"

static const char *TAG = "sysmon";

#define SYSMON_PERIOD_MS        5000
#define SYSMON_LOG_PERIOD_MS    60000
#define HEAP_WARN_BYTES         (40 * 1024)
#define HEAP_REBOOT_BYTES       (25 * 1024)
#define EXPECTED_FLASH_BYTES    (4 * 1024 * 1024)
#define CHIP_REV_SECURE_BOOT_V2 300     /* v3.0 */

#define SYSMON_STACK            3072
#define SYSMON_PRIO             1
#define SYSMON_CORE             0

static sysmon_safe_hook_t s_safe_hook;
static StaticTask_t s_task_buf;
static StackType_t s_task_stack[SYSMON_STACK];

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

        if (free_b < HEAP_REBOOT_BYTES) {
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

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(SYSMON_PERIOD_MS));
    }
}

esp_err_t sysmon_start(void)
{
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(sysmon_task, "task_sys", SYSMON_STACK, NULL,
                                                   SYSMON_PRIO, s_task_stack, &s_task_buf, SYSMON_CORE);
    return h != NULL ? ESP_OK : ESP_FAIL;
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
