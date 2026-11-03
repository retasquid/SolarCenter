#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cfg_types.h"

typedef enum {
    CFG_STATUS_OK = 0,
    CFG_STATUS_FIRST_BOOT,      /* rien en NVS : valeurs par défaut */
    CFG_STATUS_RESET_VERSION,   /* version du blob différente, pas de migration */
    CFG_STATUS_RESET_CRC,       /* blob corrompu */
    CFG_STATUS_RESET_INVALID,   /* blob hors bornes */
    CFG_STATUS_RESET_READ,      /* lecture NVS impossible */
} cfg_status_t;

/* nvs_flash_init() doit avoir été appelé avant. */
esp_err_t config_init(void);

/* Copie de la config courante. */
void config_get(cfg_t *out);

/* Valide puis enregistre. ESP_ERR_INVALID_ARG si invalide, avec *bad_field renseigné. */
esp_err_t config_save(const cfg_t *in, const char **bad_field);

cfg_status_t config_status(void);
const char *config_status_str(cfg_status_t st);

bool config_admin_is_set(void);

/* Comparaison en temps constant avec le mot de passe administrateur (faux s'il n'est pas défini). */
bool config_check_admin(const char *pass);
