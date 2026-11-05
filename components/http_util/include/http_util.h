#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

/* Une seule session TLS à la fois (CLAUDE.md §2) : pris par http_get() pour toute URL https. */
extern SemaphoreHandle_t g_tls_mutex;

typedef enum {
    HTTP_TLS_NONE = 0,      /* http:// */
    HTTP_TLS_INSECURE,      /* https:// sans vérification (certificat auto-signé de l'Envoy) */
    HTTP_TLS_BUNDLE,        /* https:// vérifié par le bundle de certificats d'IDF */
} http_tls_t;

typedef struct {
    const char *url;
    const char *bearer;     /* jeton pour « Authorization: Bearer », NULL si aucun */
    http_tls_t  tls;
    int         timeout_ms; /* 0 → 3000 ms */
} http_req_t;

esp_err_t http_util_init(void);

/* GET avec au plus 1 nouvel essai sur erreur réseau. Le corps est copié dans buf, terminé par NUL.
 *  ESP_OK                    : statut 200, *len renseigné
 *  ESP_ERR_INVALID_SIZE      : réponse plus grande que cap − 1
 *  ESP_ERR_INVALID_RESPONSE  : statut ≠ 200 (*status renseigné)
 *  autre                     : erreur réseau ou TLS */
esp_err_t http_get(const http_req_t *req, char *buf, size_t cap, size_t *len, int *status);

/* POST d'un corps JSON, mêmes règles et codes de retour que http_get. */
esp_err_t http_post_json(const http_req_t *req, const char *body, char *buf, size_t cap, size_t *len, int *status);
