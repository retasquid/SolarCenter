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

bool app_state_usable(const app_state_t *st, dev_id_t id, const meas_t *m)
{
    const dev_status_t *d = &st->dev[id];
    return m->valid && d->configured && !d->stale;
}

bool app_state_flows_in(const app_state_t *st, flows_in_t *in, bool *full)
{
    bool grid_ok = app_state_usable(st, DEV_SHELLY, &st->p_grid);
    bool pv_ok = app_state_usable(st, DEV_ENVOY, &st->p_pv);
    bool batt_ok = app_state_usable(st, DEV_ZENDURE, &st->p_batt);
    bool ce_ok = app_state_usable(st, DEV_TONGOU, &st->p_ce);
    in->p_grid = grid_ok ? st->p_grid.value : 0.0f;
    in->p_pv = pv_ok ? st->p_pv.value : 0.0f;
    in->p_batt = batt_ok ? st->p_batt.value : 0.0f;
    in->p_ce = ce_ok ? st->p_ce.value : 0.0f;
    *full = grid_ok && (pv_ok || !st->dev[DEV_ENVOY].configured) &&
            (batt_ok || !st->dev[DEV_ZENDURE].configured) &&
            (ce_ok || !st->dev[DEV_TONGOU].configured);
    return grid_ok;
}
