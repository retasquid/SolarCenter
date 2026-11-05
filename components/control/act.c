#include <math.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "app_state.h"
#include "config.h"
#include "drv_zendure.h"
#include "drv_tongou.h"
#include "sysmon.h"
#include "control_priv.h"

static const char *TAG = "act";

#define ACT_STACK           5120
#define ACT_PRIO            6
#define ACT_CORE            0
#define ACT_WAIT_MS         1000
#define ZENDURE_MIN_DELTA_W 20.0f       /* §8.3 : pas d'écriture pour un écart < 20 W */
#define ZENDURE_MIN_GAP_S   10          /* §8.3 : au plus 1 écriture toutes les 10 s */
#define RELAY_RESYNC_S      30          /* relais mesuré ≠ commande : renvoi après 30 s */

static StaticTask_t s_task_buf;
static StackType_t s_task_stack[ACT_STACK];
static cfg_t s_cfg;

static void submit(app_cmd_type_t type, float w, bool on)
{
    app_cmd_t c = { .type = type };
    if (type == CMD_BATT_SETPOINT) {
        c.batt.watts = w;
    } else {
        c.relay.on = on;
    }
    if (!app_cmd_send(&c, 0)) {
        ESP_LOGD(TAG, "file de commandes pleine");
    }
}

void control_act_submit(float batt_sp, bool relay)
{
    submit(CMD_BATT_SETPOINT, batt_sp, false);
    submit(CMD_CE_RELAY, 0.0f, relay);
}

static bool zendure_set(float w)
{
    if (s_cfg.dev.zendure_ip[0] == '\0') {
        return false;
    }
    esp_err_t err = drv_zendure_set_power(&s_cfg, w);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SolarFlow : consigne %+.0f W", w);
    }
    return err == ESP_OK;
}

static bool relay_set(bool on)
{
    if (s_cfg.dev.tongou_ip[0] == '\0') {
        return false;
    }
    esp_err_t err = drv_tongou_set(&s_cfg, on);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "chauffe-eau : relais %s", on ? "ON" : "OFF");
    } else {
        ESP_LOGW(TAG, "chauffe-eau : commande %s impossible (%s)", on ? "ON" : "OFF", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

/* §10 : avant un redémarrage contrôlé (heap critique, OTA), appareils au repos. */
static void safe_state(void)
{
    config_get(&s_cfg);
    if (s_cfg.sys.observer) {
        return;
    }
    ESP_LOGW(TAG, "mise au repos des appareils avant redémarrage");
    zendure_set(0.0f);
    relay_set(false);
}

static void act_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    bool piloting = false;
    float want_sp = 0.0f, written_sp = NAN;
    int want_relay = -1, sent_relay = -1;
    int64_t last_write = 0, last_relay = 0;

    for (;;) {
        app_cmd_t cmd;
        bool got = app_cmd_receive(&cmd, pdMS_TO_TICKS(ACT_WAIT_MS));
        esp_task_wdt_reset();
        config_get(&s_cfg);
        int64_t now = time(NULL);

        if (s_cfg.sys.observer) {
            if (piloting) {
                /* Retour en observateur : appareils au repos, plus aucune commande ensuite. */
                ESP_LOGW(TAG, "mode observateur : batterie au repos, chauffe-eau OFF");
                zendure_set(0.0f);
                relay_set(false);
                piloting = false;
            }
            continue;
        }
        if (!piloting) {
            /* Activation du pilotage : SOC min/max puis repos (§8.3), avant toute consigne. */
            ESP_LOGW(TAG, "pilotage réel activé");
            if (s_cfg.dev.zendure_ip[0] != '\0' && drv_zendure_set_soc_limits(&s_cfg) != ESP_OK) {
                ESP_LOGW(TAG, "SOC min/max non écrits");
            }
            written_sp = zendure_set(0.0f) ? 0.0f : NAN;
            last_write = now;
            want_sp = 0.0f;
            want_relay = -1;
            sent_relay = -1;
            piloting = true;
        }
        if (got) {
            if (cmd.type == CMD_BATT_SETPOINT) {
                want_sp = cmd.batt.watts;
            } else if (cmd.type == CMD_CE_RELAY) {
                want_relay = cmd.relay.on ? 1 : 0;
            }
        }

        /* Batterie : écart ≥ 20 W et 10 s depuis la dernière écriture (§8.3). */
        bool differs = isnan(written_sp) || fabsf(want_sp - written_sp) >= ZENDURE_MIN_DELTA_W;
        if (differs && now - last_write >= ZENDURE_MIN_GAP_S) {
            last_write = now;   /* même en échec : pas de rafale d'écritures */
            if (zendure_set(want_sp)) {
                written_sp = want_sp;
            }
        }

        /* Relais : à chaque changement de décision ; renvoi si l'appareil ne suit pas. */
        if (want_relay >= 0) {
            app_state_t *st = app_state_lock();
            bool measured = st->ce_relay_on;
            bool ce_ok = app_state_usable(st, DEV_TONGOU, &st->p_ce);
            app_state_unlock();
            bool resync = ce_ok && measured != (want_relay == 1) && now - last_relay >= RELAY_RESYNC_S;
            if (want_relay != sent_relay || resync) {
                last_relay = now;
                if (relay_set(want_relay == 1)) {
                    sent_relay = want_relay;
                }
            }
        }
    }
}

esp_err_t control_act_start(void)
{
    sysmon_set_safe_hook(safe_state);
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(act_task, "task_act", ACT_STACK, NULL, ACT_PRIO, s_task_stack,
                                                   &s_task_buf, ACT_CORE);
    return h != NULL ? ESP_OK : ESP_FAIL;
}
