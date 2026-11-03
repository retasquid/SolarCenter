#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "config.h"
#include "net.h"
#include "web.h"
#include "web_priv.h"

static const char *TAG = "web";

#define WWW_ROOT        "/fs/www"
#define URI_MAX         48
#define BODY_RETRIES    3

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

/* Tampons statiques : les gestionnaires s'exécutent tous dans l'unique tâche httpd. */
static char s_chunk[1024];
static char s_path[sizeof(WWW_ROOT) + URI_MAX + 4];

esp_err_t web_send_json(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        return web_send_error(req, WEB_500, "mémoire insuffisante", NULL);
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (txt == NULL) {
        return web_send_error(req, WEB_500, "mémoire insuffisante", NULL);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return err;
}

esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *msg, const char *field)
{
    char buf[192];
    if (field != NULL) {
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"field\":\"%s\"}", msg, field);
    } else {
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

bool web_check_admin(httpd_req_t *req)
{
    char pass[CFG_ADMIN_PASS_LEN];
    if (httpd_req_get_hdr_value_str(req, WEB_ADMIN_HDR, pass, sizeof(pass)) != ESP_OK) {
        return false;
    }
    bool ok = config_check_admin(pass);
    memset(pass, 0, sizeof(pass));
    if (!ok) {
        ESP_LOGW(TAG, "mot de passe administrateur refusé (%s)", req->uri);
    }
    return ok;
}

int web_read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) {
        return -1;
    }
    size_t got = 0;
    int retries = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= BODY_RETRIES) {
            continue;
        }
        if (r <= 0) {
            return -2;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    return (int)got;
}

esp_err_t web_send_setup_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    /* EMBED_TXTFILES ajoute un NUL final, exclu de la réponse. */
    return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start - 1);
}

static const char *mime_for(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext == NULL) {
        return "application/octet-stream";
    }
    if (strcmp(ext, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(ext, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

static esp_err_t send_file(httpd_req_t *req, FILE *f)
{
    size_t n;
    while ((n = fread(s_chunk, 1, sizeof(s_chunk), f)) > 0) {
        esp_err_t err = httpd_resp_send_chunk(req, s_chunk, n);
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    char uri[URI_MAX];
    size_t len = strcspn(req->uri, "?#");
    if (len >= sizeof(uri)) {
        return web_send_error(req, WEB_404, "introuvable", NULL);
    }
    memcpy(uri, req->uri, len);
    uri[len] = '\0';
    if (strstr(uri, "..") != NULL) {
        return web_send_error(req, WEB_400, "chemin invalide", NULL);
    }

    if (strcmp(uri, "/setup") == 0 || (strcmp(uri, "/") == 0 && net_is_setup_mode())) {
        return web_send_setup_page(req);
    }
    if (strcmp(uri, "/") == 0) {
        strcpy(uri, "/index.html");
    }

    bool gz = true;
    snprintf(s_path, sizeof(s_path), WWW_ROOT "%s.gz", uri);
    FILE *f = fopen(s_path, "rb");
    if (f == NULL) {
        gz = false;
        snprintf(s_path, sizeof(s_path), WWW_ROOT "%s", uri);
        f = fopen(s_path, "rb");
    }
    if (f == NULL) {
        if (strcmp(uri, "/index.html") == 0) {
            /* Interface absente de LittleFS : le portail reste accessible. */
            ESP_LOGW(TAG, "interface web absente (python tools/build_web.py puis flash)");
            return web_send_setup_page(req);
        }
        return web_send_error(req, WEB_404, "introuvable", NULL);
    }

    httpd_resp_set_type(req, mime_for(uri));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    esp_err_t err = send_file(req, f);
    fclose(f);
    return err;
}

esp_err_t web_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 8192;
    c.task_priority = 5;
    c.core_id = 0;
    c.max_uri_handlers = 16;
    c.lru_purge_enable = true;
    c.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t srv = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&srv, &c), TAG, "httpd_start");
    ESP_RETURN_ON_ERROR(api_register(srv), TAG, "API");

    /* Enregistré en dernier : le joker ne doit pas masquer /api/. */
    const httpd_uri_t st = { .uri = "/*", .method = HTTP_GET, .handler = static_get_handler };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(srv, &st), TAG, "fichiers statiques");

    ESP_LOGI(TAG, "serveur HTTP démarré");
    return ESP_OK;
}
