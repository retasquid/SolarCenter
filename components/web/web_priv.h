#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"
#include "cJSON.h"
#include "cfg_types.h"

#define WEB_ADMIN_HDR   "X-Admin-Password"

#define WEB_400 "400 Bad Request"
#define WEB_401 "401 Unauthorized"
#define WEB_403 "403 Forbidden"
#define WEB_404 "404 Not Found"
#define WEB_413 "413 Payload Too Large"
#define WEB_500 "500 Internal Server Error"

/* Envoie root en JSON puis le libère. */
esp_err_t web_send_json(httpd_req_t *req, cJSON *root);

/* {"error":msg,"field":field}, field peut être NULL. msg et field ne contiennent pas de guillemets. */
esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *msg, const char *field);

bool web_check_admin(httpd_req_t *req);

/* Lit le corps dans buf (terminé par NUL). Retourne la longueur, -1 si trop grand, -2 si erreur. */
int web_read_body(httpd_req_t *req, char *buf, size_t cap);

esp_err_t web_send_setup_page(httpd_req_t *req);

esp_err_t api_register(httpd_handle_t srv);

/* État instantané (mesures, flux, appareils), partagé par /api/state et /ws. */
cJSON *state_to_json(void);

/* Historique, prévisions, statistiques, réinitialisation des modèles. */
esp_err_t api_hist_register(httpd_handle_t srv);

/* Plan, journal des décisions, forçage du chauffe-eau. */
esp_err_t api_ctl_register(httpd_handle_t srv);

/* Écriture JSON par morceaux (réponses longues sans arbre cJSON en mémoire). */
typedef struct {
    httpd_req_t *req;
    char         buf[1024];
    size_t       len;
    esp_err_t    err;
} jw_t;

void jw_begin(jw_t *w, httpd_req_t *req);
void jw_printf(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jw_num(jw_t *w, float v, int decimals);     /* "null" si non fini */
esp_err_t jw_end(jw_t *w);

/* Conversion de la config ; les secrets sont renvoyés "***" s'ils sont définis. */
cJSON *cfg_to_json(const cfg_t *c);

/* Mise à jour partielle : les clés absentes restent inchangées, "***" conserve un secret.
 * Retourne NULL ou le champ fautif. */
const char *cfg_from_json(const cJSON *root, cfg_t *c);

/* Copie une chaîne JSON optionnelle ; faux si mauvais type ou trop longue. */
bool json_get_str(const cJSON *obj, const char *key, char *dst, size_t cap, bool secret);
