#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cfg_types.h"

/* WiFi STA (ou portail SoftAP « Solaire-Setup » si aucun réseau n'est configuré), NTP, mDNS.
 * esp_netif_init() et esp_event_loop_create_default() doivent avoir été appelés. */
esp_err_t net_start(const cfg_t *cfg);

/* Vrai si aucun réseau WiFi n'est configuré : seul le portail est disponible. */
bool net_is_setup_mode(void);

/* RSSI du point d'accès en dBm, 0 si non connecté. */
int8_t net_rssi(void);
