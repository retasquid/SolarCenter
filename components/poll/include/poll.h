#pragma once

#include "esp_err.h"

/* task_poll : lectures séquentielles des appareils (CLAUDE.md §6) et mise à jour de l'état partagé. */
esp_err_t poll_start(void);

/* Périodes de sondage, ms. Shelly : cfg.sys.poll_ms. */
#define POLL_ZENDURE_MS     5000
#define POLL_TONGOU_MS      5000
#define POLL_ENVOY_MS       10000
