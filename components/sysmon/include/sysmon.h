#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Mise des appareils dans leur état de repli (fourni à partir du M4). */
typedef void (*sysmon_safe_hook_t)(void);

/* Version, révision de puce, taille de flash, cause du dernier redémarrage. */
void sysmon_log_boot_info(void);

/* Tâche de surveillance : heap (journal toutes les 60 s, seuils §10), RSSI. */
esp_err_t sysmon_start(void);

void sysmon_set_safe_hook(sysmon_safe_hook_t hook);

/* Redémarrage contrôlé : état de repli des appareils, puis esp_restart(). */
void sysmon_restart(const char *reason);

/* Même chose depuis task_sys, 1,5 s plus tard (réponse HTTP envoyée, pas d'appel réseau dans httpd).
 * reason doit rester valide (chaîne littérale). */
void sysmon_request_restart(const char *reason);

/* Écriture OTA en cours : seuil de redémarrage « heap critique » abaissé (l'OTA abandonne avant). */
void sysmon_set_ota_active(bool active);
