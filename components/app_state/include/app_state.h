#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

/* Mesure horodatée commune à tous les pilotes (CLAUDE.md §8). */
typedef struct {
    float    value;
    uint32_t ts_ms;     /* horloge monotone, ms depuis le démarrage (voir app_now_ms) */
    bool     valid;
} meas_t;

typedef enum {
    DEV_SHELLY = 0,
    DEV_ENVOY,
    DEV_ZENDURE,
    DEV_TONGOU,
    DEV_COUNT
} dev_id_t;

typedef struct {
    bool     configured;        /* adresse IP renseignée */
    bool     ever_ok;           /* au moins une lecture réussie depuis le démarrage */
    uint32_t last_ok_ms;        /* dernière lecture réussie */
    uint32_t err_count;         /* erreurs cumulées depuis le démarrage */
    uint8_t  consecutive_err;
    int32_t  last_err;          /* esp_err_t de la dernière erreur */
    bool     stale;             /* plus de 3 périodes sans lecture valide */
} dev_status_t;

typedef struct {
    int   ac_mode;
    float input_limit_w;
    float output_limit_w;
    int   soc_set_pm;           /* ‰ */
    int   min_soc_pm;           /* ‰ */
} zendure_info_t;

typedef struct {
    /* Mesures */
    meas_t p_grid;
    meas_t p_pv;
    meas_t p_batt;
    meas_t p_ce;
    meas_t soc;
    float  p_grid_phase[3];
    bool   pv_from_eim;         /* faux : repli sur la production des micro-onduleurs */
    bool   ce_relay_on;
    zendure_info_t zendure;
    dev_status_t dev[DEV_COUNT];
    bool   shelly_fallback;     /* Shelly invalide depuis plus de 30 s : mode repli (§8.1, §10) */

    /* Réseau */
    bool   wifi_connected;
    bool   ap_active;           /* portail SoftAP ouvert (premier démarrage ou secours) */
    int8_t rssi;
    char   ip[16];

    /* Heure */
    bool    time_synced;        /* au moins une synchro NTP depuis le démarrage */
    int64_t last_ntp_sync;      /* epoch (s) de la dernière synchro, 0 si aucune */

    /* Mémoire */
    uint32_t heap_free;
    uint32_t heap_min;
} app_state_t;

/* Commandes vers la tâche d'action (file q_cmd). */
typedef enum {
    CMD_BATT_SETPOINT = 0,      /* consigne batterie, W (> 0 décharge, < 0 charge) */
    CMD_CE_RELAY,               /* relais du chauffe-eau */
    CMD_CE_OVERRIDE,            /* forçage utilisateur du chauffe-eau */
} app_cmd_type_t;

typedef enum {
    OVR_AUTO = 0,
    OVR_ON,
    OVR_OFF,
} ovr_mode_t;

typedef struct {
    app_cmd_type_t type;
    union {
        struct { float watts; } batt;
        struct { bool on; } relay;
        struct { ovr_mode_t mode; uint16_t minutes; } ovr;
    };
} app_cmd_t;

esp_err_t app_state_init(void);

/* Lecteurs : copie locale sous verrou, le verrou est relâché immédiatement. */
void app_state_get(app_state_t *out);

/* Écrivains : section courte, jamais d'appel réseau entre lock et unlock. */
app_state_t *app_state_lock(void);
void app_state_unlock(void);

bool app_cmd_send(const app_cmd_t *cmd, TickType_t wait);
bool app_cmd_receive(app_cmd_t *cmd, TickType_t wait);

/* Horloge monotone en ms (reboucle après ~49 jours : comparer par différence non signée). */
uint32_t app_now_ms(void);
