#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "app_state.h"

static const char *TAG = "app_state";

#define CMD_QUEUE_LEN 8

static app_state_t s_state;

static StaticSemaphore_t s_mtx_buf;
static SemaphoreHandle_t s_mtx;

static StaticQueue_t s_q_buf;
static uint8_t s_q_storage[CMD_QUEUE_LEN * sizeof(app_cmd_t)];
static QueueHandle_t q_cmd;

esp_err_t app_state_init(void)
{
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    q_cmd = xQueueCreateStatic(CMD_QUEUE_LEN, sizeof(app_cmd_t), s_q_storage, &s_q_buf);
    if (s_mtx == NULL || q_cmd == NULL) {
        return ESP_FAIL;
    }
    memset(&s_state, 0, sizeof(s_state));
    ESP_LOGI(TAG, "état partagé prêt (%u octets)", (unsigned)sizeof(s_state));
    return ESP_OK;
}

void app_state_get(app_state_t *out)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(out, &s_state, sizeof(*out));
    xSemaphoreGive(s_mtx);
}

app_state_t *app_state_lock(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    return &s_state;
}

void app_state_unlock(void)
{
    xSemaphoreGive(s_mtx);
}

bool app_cmd_send(const app_cmd_t *cmd, TickType_t wait)
{
    return xQueueSend(q_cmd, cmd, wait) == pdTRUE;
}

bool app_cmd_receive(app_cmd_t *cmd, TickType_t wait)
{
    return xQueueReceive(q_cmd, cmd, wait) == pdTRUE;
}

uint32_t app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
