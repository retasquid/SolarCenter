#include <math.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "control.h"
#include "web_priv.h"

static const char *TAG = "web";

/* Tampons statiques : tous les gestionnaires s'exécutent dans l'unique tâche httpd. */
static control_status_t s_st;

#define BODY_MAX    128

_Static_assert(CONTROL_LOG_LEN * sizeof(ctl_log_t) <= WEB_SCRATCH_LEN, "journal plus grand que g_web_scratch");

static const char *ovr_name(uint8_t o)
{
    return o == REG_OVR_ON ? "on" : o == REG_OVR_OFF ? "off" : "auto";
}

/* ---------- GET /api/plan ---------- */

static esp_err_t plan_get(httpd_req_t *req)
{
    control_status(&s_st);
    jw_t *w = &g_web_jw;
    jw_begin(w, req);
    jw_printf(w, "{\"running\":%s,\"observer\":%s,\"plan_valid\":%s,\"updated\":%lld,",
              s_st.running ? "true" : "false", s_st.observer ? "true" : "false",
              s_st.plan_valid ? "true" : "false", (long long)s_st.updated);
    const plan_t *p = &s_st.plan;
    jw_printf(w, "\"mode\":\"%s\",\"S_wh\":%.0f,\"S_shower_wh\":%.0f,\"B_wh\":%.0f,\"E_ce_wh\":%.0f,",
              planner_mode_name(p->mode), (double)p->S_wh, (double)p->S_shower_wh, (double)p->B_wh,
              (double)p->E_ce_wh);
    jw_printf(w, "\"values\":{\"batt\":%.4f,\"ce\":%.4f,\"sell\":%.4f},\"ce_first\":%s,",
              (double)p->v_batt, (double)p->v_ce, (double)p->v_sell, p->ce_first ? "true" : "false");
    jw_printf(w, "\"t0\":%lld,\"slot_s\":%d,\"soc_target_now\":", (long long)p->t0, PLAN_SLOT_S);
    jw_num(w, s_st.soc_target, 1);
    jw_printf(w, ",\"soc_target\":[");
    for (int i = 0; i < p->n; i++) {
        jw_printf(w, "%s%.1f", i ? "," : "", (double)p->soc_target[i]);
    }
    const wh_night_t *n = &s_st.night;
    jw_printf(w, "],\"night\":{\"active\":%s,\"start\":%lld,\"end\":%lld,\"e_wh\":%.0f,\"full\":%s},",
              n->active ? "true" : "false", (long long)n->start, (long long)n->end, (double)n->e_target_wh,
              n->full ? "true" : "false");
    const reg_output_t *o = &s_st.out;
    jw_printf(w, "\"wh\":{\"tank_full\":%s,\"last_full\":%lld,\"e_full_wh\":%.0f,\"e_heated_wh\":%.0f,"
                 "\"catchup_at\":%lld,\"catchup_blocked\":%s,\"last_shower\":%lld,\"next_shower\":%lld},",
              s_st.tank_full ? "true" : "false", (long long)s_st.last_full, (double)s_st.e_full_wh,
              (double)s_st.e_heated_wh, (long long)o->catchup_at, o->catchup_blocked ? "true" : "false",
              (long long)s_st.last_shower, (long long)s_st.next_shower);
    jw_printf(w, "\"decision\":{\"relay\":%s,\"ce_reason\":\"%s\",\"batt_sp\":%.0f,\"batt_reason\":\"%s\","
                 "\"limits\":%u,\"avail_w\":%.0f},",
              o->relay ? "true" : "false", regulator_ce_reason_name(o->ce_reason), (double)o->batt_sp,
              regulator_batt_reason_name(o->batt_reason), o->batt_limits, (double)o->avail_w);
    jw_printf(w, "\"override\":{\"mode\":\"%s\",\"until\":%lld}}", ovr_name(s_st.override),
              (long long)s_st.override_until);
    return jw_end(w);
}

/* ---------- GET /api/log ---------- */

static esp_err_t log_get(httpd_req_t *req)
{
    ctl_log_t *log = (ctl_log_t *)g_web_scratch;
    int n = control_log_read(log, CONTROL_LOG_LEN);
    jw_t *w = &g_web_jw;
    jw_begin(w, req);
    jw_printf(w, "{\"lines\":[");
    for (int i = 0; i < n; i++) {
        const ctl_log_t *e = &log[i];
        jw_printf(w, "%s{\"t\":%lu,\"obs\":%s,\"relay\":%s,\"ce\":\"%s\",\"batt\":\"%s\",\"sp\":%d,"
                       "\"grid\":%d,\"pbatt\":%d,\"avail\":%d,\"lim\":%u,\"mode\":\"%s\",\"hc\":%s,\"full\":%s,"
                       "\"fallback\":%s,\"soc\":",
                  i ? "," : "", (unsigned long)e->ts, (e->flags & CTL_F_OBSERVER) ? "true" : "false",
                  (e->flags & CTL_F_RELAY) ? "true" : "false", regulator_ce_reason_name(e->ce_reason),
                  regulator_batt_reason_name(e->batt_reason), e->batt_sp, e->p_grid, e->p_batt, e->avail,
                  e->limits, planner_mode_name((ce_mode_t)e->mode), (e->flags & CTL_F_HC) ? "true" : "false",
                  (e->flags & CTL_F_FULL) ? "true" : "false", (e->flags & CTL_F_FALLBACK) ? "true" : "false");
        if (e->soc <= 100) {
            jw_printf(w, "%u}", e->soc);
        } else {
            jw_printf(w, "null}");
        }
    }
    jw_printf(w, "]}");
    return jw_end(w);
}

/* ---------- POST /api/override ---------- */

static esp_err_t override_post(httpd_req_t *req)
{
    char *body = (char *)g_web_scratch;
    int n = web_read_body(req, body, BODY_MAX);
    if (n < 0) {
        return web_send_error(req, WEB_400, "corps invalide", NULL);
    }
    cJSON *root = cJSON_ParseWithLength(body, (size_t)n);
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *minutes = cJSON_GetObjectItemCaseSensitive(root, "minutes");
    const char *bad = NULL;
    reg_override_t m = REG_OVR_AUTO;
    uint16_t min = 0;
    if (!cJSON_IsString(target) || strcmp(target->valuestring, "ce") != 0) {
        bad = "target";
    } else if (!cJSON_IsString(mode)) {
        bad = "mode";
    } else if (strcmp(mode->valuestring, "on") == 0) {
        m = REG_OVR_ON;
    } else if (strcmp(mode->valuestring, "off") == 0) {
        m = REG_OVR_OFF;
    } else if (strcmp(mode->valuestring, "auto") != 0) {
        bad = "mode";
    }
    if (bad == NULL && m != REG_OVR_AUTO) {
        if (!cJSON_IsNumber(minutes) || minutes->valuedouble < 1 || minutes->valuedouble > 1440) {
            bad = "minutes";
        } else {
            min = (uint16_t)minutes->valuedouble;
        }
    }
    cJSON_Delete(root);
    if (bad != NULL) {
        return web_send_error(req, WEB_400, "valeur invalide", bad);
    }
    if (control_override(m, min) != ESP_OK) {
        return web_send_error(req, WEB_400, "forçage refusé", NULL);
    }
    cJSON *r = cJSON_CreateObject();
    if (r != NULL) {
        cJSON_AddBoolToObject(r, "ok", true);
    }
    return web_send_json(req, r);
}

esp_err_t api_ctl_register(httpd_handle_t srv)
{
    const httpd_uri_t routes[] = {
        { .uri = "/api/plan",     .method = HTTP_GET,  .handler = plan_get },
        { .uri = "/api/log",      .method = HTTP_GET,  .handler = log_get },
        { .uri = "/api/override", .method = HTTP_POST, .handler = override_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(srv, &routes[i]), TAG, "%s", routes[i].uri);
    }
    return ESP_OK;
}
