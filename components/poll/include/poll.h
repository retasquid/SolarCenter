#pragma once

#include "esp_err.h"

/* task_grid  : Shelly seul (capteur maître, jamais bloqué par un autre appareil).
 * task_poll  : Zendure puis Tongou (HTTP local).
 * task_envoy : Envoy seul (HTTPS, réponse lente).
 * Constat du 2026-11-04 : avec une seule tâche séquentielle (CLAUDE.md §6), l'Envoy et un
 * appareil injoignable périmaient le Shelly. Toutes mettent à jour l'état partagé. */
esp_err_t poll_start(void);

/* Périodes de sondage, ms. Shelly : cfg.sys.poll_ms. */
#define POLL_ZENDURE_MS     2000    /* au rythme du Shelly : p_batt et p_grid lus au même moment */
#define POLL_TONGOU_MS      5000
#define POLL_ENVOY_MS       10000
