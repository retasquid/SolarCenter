#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "web_priv.h"

#define JW_FLUSH_MARGIN 160     /* place minimale avant d'écrire un élément */

static void flush(jw_t *w)
{
    if (w->len > 0 && w->err == ESP_OK) {
        w->err = httpd_resp_send_chunk(w->req, w->buf, w->len);
    }
    w->len = 0;
}

void jw_begin(jw_t *w, httpd_req_t *req)
{
    w->req = req;
    w->len = 0;
    w->err = ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

void jw_printf(jw_t *w, const char *fmt, ...)
{
    if (sizeof(w->buf) - w->len < JW_FLUSH_MARGIN) {
        flush(w);
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, sizeof(w->buf) - w->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(w->buf) - w->len) {
        /* Élément trop long pour la place restante : on vide puis on réécrit. */
        flush(w);
        va_start(ap, fmt);
        n = vsnprintf(w->buf, sizeof(w->buf), fmt, ap);
        va_end(ap);
        if (n < 0 || (size_t)n >= sizeof(w->buf)) {
            w->err = ESP_ERR_INVALID_SIZE;
            return;
        }
    }
    w->len += (size_t)n;
}

void jw_num(jw_t *w, float v, int decimals)
{
    if (!isfinite(v)) {
        jw_printf(w, "null");
    } else {
        jw_printf(w, "%.*f", decimals, (double)v);
    }
}

esp_err_t jw_end(jw_t *w)
{
    flush(w);
    if (w->err == ESP_OK) {
        w->err = httpd_resp_send_chunk(w->req, NULL, 0);
    }
    return w->err;
}
