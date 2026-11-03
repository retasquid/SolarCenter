#pragma once

#include "esp_err.h"

/* Serveur HTTP : interface web (LittleFS, monté sur /fs), portail de configuration, API REST. */
esp_err_t web_start(void);
