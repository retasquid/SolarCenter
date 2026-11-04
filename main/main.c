#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_littlefs.h"
#include "app_state.h"
#include "config.h"
#include "net.h"
#include "web.h"
#include "sysmon.h"
#include "http_util.h"
#include "poll.h"
#include "forecast.h"
#include "history.h"
#include "drv_tuya.h"

static const char *TAG = "main";

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "partition NVS réinitialisée (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* Non fatal : sans LittleFS, le portail et l'API restent disponibles. */
static void storage_mount(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = "/fs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS non monté : %s", esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    if (esp_littlefs_info(conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS : %u / %u Ko utilisés", (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
}

void app_main(void)
{
    nvs_init();
    ESP_ERROR_CHECK(app_state_init());
    ESP_ERROR_CHECK(config_init());
    ESP_ERROR_CHECK(http_util_init());
    sysmon_log_boot_info();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    storage_mount();

    static cfg_t cfg;
    config_get(&cfg);
    ESP_ERROR_CHECK(net_start(&cfg));
    ESP_ERROR_CHECK(web_start());
    ESP_ERROR_CHECK(sysmon_start());
    if (!net_is_setup_mode()) {
        drv_tuya_selftest();
        ESP_ERROR_CHECK(forecast_start());
        ESP_ERROR_CHECK(history_start());
        ESP_ERROR_CHECK(poll_start());
    }
}
