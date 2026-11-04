#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cfg_types.h"

/* Interrupteur sous firmware Tuya d'origine, protocole local chiffré 3.3 / 3.4 (TCP 6668).
 * Adresse : cfg->dev.tongou_ip ; identifiant, clé, version et DP : cfg->tuya. */
typedef struct {
    bool  relay_on;
    float power_w;
} tuya_reading_t;

esp_err_t drv_tuya_poll(const cfg_t *cfg, tuya_reading_t *out);

/* Commande du relais (pilotage réel : M4 seulement, inutilisé en mode observateur). */
esp_err_t drv_tuya_set(const cfg_t *cfg, bool on);

/* Vérifie AES et HMAC (PSA) contre les valeurs de la référence Python ; résultat dans le journal. */
void drv_tuya_selftest(void);
