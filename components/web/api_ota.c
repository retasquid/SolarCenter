#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_check.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "config.h"
#include "http_util.h"
#include "sysmon.h"
#include "web_priv.h"

static const char *TAG = "ota";

#define WWW_DIR         "/fs/www"
#define WWW_NAME_MAX    32
#define WWW_FILE_MAX    (64 * 1024)
#define RECV_RETRIES    5
#define OTA_TLS_WAIT_MS 20000
#define OTA_LOG_STEP    (128 * 1024)
#define OTA_HEAP_MIN    (28 * 1024)     /* abandon propre avant le redémarrage « heap critique » */
/* En-tête d'image, premier segment, puis description de l'application. */
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HEAD_LEN        (APP_DESC_OFFSET + sizeof(esp_app_desc_t))

/* Tampons statiques : les gestionnaires s'exécutent tous dans l'unique tâche httpd. */
static char s_buf[2048];
static char s_path[sizeof(WWW_DIR) + WWW_NAME_MAX + 8];
static char s_tmp[sizeof(s_path)];

static bool check_admin(httpd_req_t *req, esp_err_t *err)
{
    if (!config_admin_is_set()) {
        *err = web_send_error(req, WEB_403, "mot de passe administrateur non défini", "sys.admin_pass");
        return false;
    }
    if (!web_check_admin(req)) {
        *err = web_send_error(req, WEB_401, "mot de passe administrateur invalide", NULL);
        return false;
    }
    return true;
}

/* Lit jusqu'à cap octets du corps ; 0 en fin de corps, < 0 en cas d'erreur. */
static int recv_some(httpd_req_t *req, char *buf, size_t cap)
{
    for (int retries = 0; retries <= RECV_RETRIES; retries++) {
        int r = httpd_req_recv(req, buf, cap);
        if (r != HTTPD_SOCK_ERR_TIMEOUT) {
            return r;
        }
    }
    return -1;
}

/* Refuse une image qui n'est pas un firmware de ce projet pour ESP32. */
static const char *check_image(const char *head, size_t len)
{
    if (len < HEAD_LEN) {
        return "image trop courte";
    }
    const esp_image_header_t *h = (const esp_image_header_t *)head;
    if (h->magic != ESP_IMAGE_HEADER_MAGIC) {
        return "pas une image ESP (fichier .bin de build/ attendu)";
    }
    if (h->chip_id != ESP_CHIP_ID_ESP32) {
        return "image pour une autre puce";
    }
    esp_app_desc_t desc;
    memcpy(&desc, head + APP_DESC_OFFSET, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "description de l'application absente";
    }
    if (strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) != 0) {
        return "firmware d'un autre projet";
    }
    ESP_LOGI(TAG, "image %.32s v%.32s du %.16s %.16s", desc.project_name, desc.version, desc.date, desc.time);
    return NULL;
}

/* ---------- POST /api/ota : corps = solar_center.bin brut ---------- */

static esp_err_t ota_post(httpd_req_t *req)
{
    esp_err_t err = ESP_OK;
    if (!check_admin(req, &err)) {
        return err;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return web_send_error(req, WEB_500, "aucune partition OTA libre", NULL);
    }
    if (req->content_len < HEAD_LEN) {
        return web_send_error(req, WEB_400, "image trop courte", NULL);
    }
    if (req->content_len > part->size) {
        return web_send_error(req, WEB_413, "image plus grande que la partition", NULL);
    }

    /* Début de l'image vérifié avant toute écriture en flash. */
    size_t head = 0;
    while (head < HEAD_LEN) {
        int r = recv_some(req, s_buf + head, sizeof(s_buf) - head);
        if (r <= 0) {
            return web_send_error(req, WEB_400, "réception interrompue", NULL);
        }
        head += (size_t)r;
    }
    const char *bad = check_image(s_buf, head);
    if (bad != NULL) {
        ESP_LOGW(TAG, "image refusée : %s", bad);
        return web_send_error(req, WEB_400, bad, NULL);
    }

    /* Aucune session TLS (Envoy, Open-Meteo, ~40 Ko de heap) pendant l'écriture. */
    if (xSemaphoreTake(g_tls_mutex, pdMS_TO_TICKS(OTA_TLS_WAIT_MS)) != pdTRUE) {
        return web_send_error(req, WEB_503, "session TLS en cours, réessayer", NULL);
    }
    ESP_LOGW(TAG, "mise à jour du firmware vers %s (%u octets), heap %u o", part->label,
             (unsigned)req->content_len, (unsigned)esp_get_free_heap_size());
    sysmon_set_ota_active(true);
    esp_ota_handle_t h;
    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        sysmon_set_ota_active(false);
        xSemaphoreGive(g_tls_mutex);
        ESP_LOGE(TAG, "esp_ota_begin : %s", esp_err_to_name(err));
        return web_send_error(req, WEB_500, "écriture en flash impossible", NULL);
    }
    size_t done = 0;
    size_t chunk = head;
    size_t next_log = OTA_LOG_STEP;
    bool low_heap = false;
    while (err == ESP_OK) {
        err = esp_ota_write(h, s_buf, chunk);
        done += chunk;
        if (err != ESP_OK || done >= req->content_len) {
            break;
        }
        uint32_t heap = esp_get_free_heap_size();
        if (done >= next_log) {
            next_log += OTA_LOG_STEP;
            ESP_LOGI(TAG, "%u / %u Ko, heap %u o", (unsigned)(done / 1024), (unsigned)(req->content_len / 1024),
                     (unsigned)heap);
        }
        if (heap < OTA_HEAP_MIN) {
            low_heap = true;
            err = ESP_ERR_NO_MEM;
            break;
        }
        vTaskDelay(1);      /* laisse tourner les tâches de priorité inférieure (watchdog de l'idle) */
        size_t want = req->content_len - done;
        int r = recv_some(req, s_buf, want < sizeof(s_buf) ? want : sizeof(s_buf));
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        chunk = (size_t)r;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "écriture interrompue à %u octets : %s, heap %u o", (unsigned)done, esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size());
        esp_ota_abort(h);
        sysmon_set_ota_active(false);
        xSemaphoreGive(g_tls_mutex);
        return web_send_error(req, low_heap ? WEB_503 : WEB_400,
                              low_heap ? "mémoire insuffisante, firmware actuel conservé"
                                       : "réception ou écriture interrompue, firmware actuel conservé", NULL);
    }
    err = esp_ota_end(h);     /* vérifie l'empreinte de l'image */
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    sysmon_set_ota_active(false);
    xSemaphoreGive(g_tls_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image invalide : %s", esp_err_to_name(err));
        return web_send_error(req, WEB_400, "image invalide, firmware actuel conservé", NULL);
    }

    ESP_LOGW(TAG, "firmware écrit dans %s : redémarrage", part->label);
    cJSON *r = cJSON_CreateObject();
    if (r != NULL) {
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddStringToObject(r, "partition", part->label);
    }
    err = web_send_json(req, r);
    sysmon_request_restart("mise à jour du firmware");
    return err;
}

/* ---------- POST /api/www?file=app.js.gz : un fichier de l'interface ---------- */

/* Nom simple : lettres, chiffres, '-', '_', '.', terminé par .gz, sans chemin. */
static bool www_name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len < 4 || len >= WWW_NAME_MAX || n[0] == '.' || strcmp(n + len - 3, ".gz") != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return strstr(n, "..") == NULL;
}

static esp_err_t www_post(httpd_req_t *req)
{
    esp_err_t err = ESP_OK;
    if (!check_admin(req, &err)) {
        return err;
    }
    char q[64];
    char name[WWW_NAME_MAX];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "file", name, sizeof(name)) != ESP_OK || !www_name_ok(name)) {
        return web_send_error(req, WEB_400, "nom de fichier invalide (ex. app.js.gz)", "file");
    }
    if (req->content_len == 0 || req->content_len > WWW_FILE_MAX) {
        return web_send_error(req, WEB_413, "fichier vide ou trop volumineux", NULL);
    }

    struct stat sb;
    if (stat(WWW_DIR, &sb) != 0 && mkdir(WWW_DIR, 0775) != 0) {
        return web_send_error(req, WEB_500, "dossier de l'interface inaccessible", NULL);
    }
    snprintf(s_path, sizeof(s_path), WWW_DIR "/%s", name);
    snprintf(s_tmp, sizeof(s_tmp), WWW_DIR "/%s.tmp", name);

    /* Écrit à côté puis remplace : une coupure ne laisse jamais un fichier à moitié écrit. */
    FILE *f = fopen(s_tmp, "wb");
    if (f == NULL) {
        return web_send_error(req, WEB_500, "écriture impossible", NULL);
    }
    size_t done = 0;
    bool ok = true;
    while (done < req->content_len) {
        size_t want = req->content_len - done;
        int r = recv_some(req, s_buf, want < sizeof(s_buf) ? want : sizeof(s_buf));
        if (r <= 0 || fwrite(s_buf, 1, (size_t)r, f) != (size_t)r) {
            ok = false;
            break;
        }
        done += (size_t)r;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (ok && rename(s_tmp, s_path) != 0) {
        /* Selon le système de fichiers, rename() ne remplace pas une cible existante. */
        ok = remove(s_path) == 0 && rename(s_tmp, s_path) == 0;
    }
    if (!ok) {
        remove(s_tmp);
        ESP_LOGE(TAG, "%s : écriture interrompue à %u octets", name, (unsigned)done);
        return web_send_error(req, WEB_500, "écriture interrompue (espace plein ?)", NULL);
    }
    ESP_LOGI(TAG, "interface : %s mis à jour (%u octets)", name, (unsigned)done);

    cJSON *r = cJSON_CreateObject();
    if (r != NULL) {
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddStringToObject(r, "file", name);
        cJSON_AddNumberToObject(r, "size", (double)done);
    }
    return web_send_json(req, r);
}

esp_err_t api_ota_register(httpd_handle_t srv)
{
    const httpd_uri_t routes[] = {
        { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post },
        { .uri = "/api/www", .method = HTTP_POST, .handler = www_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(srv, &routes[i]), TAG, "%s", routes[i].uri);
    }
    return ESP_OK;
}
