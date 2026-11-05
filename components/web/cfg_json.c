#include <stdio.h>
#include <string.h>
#include <math.h>
#include "web_priv.h"

#define SECRET_MASK "***"

#define TRY(cond, field) do { if (!(cond)) { return (field); } } while (0)

/* ---------- Écriture ---------- */

/* Arrondi à 4 décimales pour éviter « 0.87999999523 » en sortie. */
static void add_num(cJSON *o, const char *k, float v)
{
    cJSON_AddNumberToObject(o, k, (double)lroundf(v * 10000.0f) / 10000.0);
}

static void add_hhmm(cJSON *o, const char *k, uint16_t min)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(min / 60), (unsigned)(min % 60));
    cJSON_AddStringToObject(o, k, buf);
}

static void add_secret(cJSON *o, const char *k, const char *v)
{
    cJSON_AddStringToObject(o, k, v[0] != '\0' ? SECRET_MASK : "");
}

cJSON *cfg_to_json(const cfg_t *c)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *t = cJSON_AddObjectToObject(r, "tariff");
    cJSON *pv = cJSON_AddObjectToObject(r, "pv");
    cJSON *b = cJSON_AddObjectToObject(r, "batt");
    cJSON *w = cJSON_AddObjectToObject(r, "wh");
    cJSON *roi = cJSON_AddObjectToObject(r, "roi");
    cJSON *d = cJSON_AddObjectToObject(r, "dev");
    cJSON *s = cJSON_AddObjectToObject(r, "sys");
    cJSON *hc = cJSON_AddArrayToObject(t, "hc");
    cJSON *mf = cJSON_AddArrayToObject(roi, "month_frac");
    if (r == NULL || t == NULL || pv == NULL || b == NULL || w == NULL || roi == NULL ||
        d == NULL || s == NULL || hc == NULL || mf == NULL) {
        cJSON_Delete(r);
        return NULL;
    }

    cJSON_AddStringToObject(t, "type", c->tariff.type == TARIFF_BASE ? "base" : "hphc");
    cJSON_AddBoolToObject(t, "configured", c->tariff.configured);
    for (int i = 0; i < c->tariff.n_hc; i++) {
        char buf[24];
        const cfg_slot_t *sl = &c->tariff.hc[i];
        snprintf(buf, sizeof(buf), "%02u:%02u-%02u:%02u",
                 (unsigned)(sl->start_min / 60), (unsigned)(sl->start_min % 60),
                 (unsigned)(sl->end_min / 60), (unsigned)(sl->end_min % 60));
        cJSON_AddItemToArray(hc, cJSON_CreateString(buf));
    }
    add_num(t, "price_hp", c->tariff.price_hp);
    add_num(t, "price_hc", c->tariff.price_hc);
    add_num(t, "price_base", c->tariff.price_base);
    add_num(t, "price_sell", c->tariff.price_sell);

    add_num(pv, "kwc", c->pv.kwc);
    add_num(pv, "lat", c->pv.lat);
    add_num(pv, "lon", c->pv.lon);
    add_num(pv, "tilt", c->pv.tilt);
    add_num(pv, "azimuth", c->pv.azimuth);

    add_num(b, "capa_kwh", c->batt.capa_kwh);
    add_num(b, "p_charge_max", c->batt.p_charge_max);
    add_num(b, "p_discharge_max", c->batt.p_discharge_max);
    add_num(b, "eta", c->batt.eta);
    cJSON_AddNumberToObject(b, "soc_min", c->batt.soc_min);
    cJSON_AddNumberToObject(b, "soc_max", c->batt.soc_max);
    add_num(b, "price_eur", c->batt.price_eur);
    cJSON_AddNumberToObject(b, "cycles", c->batt.cycles);
    if (c->batt.commissioning != 0) {
        char buf[24];
        uint32_t dt = c->batt.commissioning;
        snprintf(buf, sizeof(buf), "%04lu-%02lu-%02lu", (unsigned long)(dt / 10000),
                 (unsigned long)((dt / 100) % 100), (unsigned long)(dt % 100));
        cJSON_AddStringToObject(b, "commissioning", buf);
    } else {
        cJSON_AddStringToObject(b, "commissioning", "");
    }
    cJSON_AddBoolToObject(b, "arbitrage", c->batt.arbitrage);

    add_num(w, "power_w", c->wh.power_w);
    cJSON_AddNumberToObject(w, "full_heat_min", c->wh.full_heat_min);
    add_hhmm(w, "shower", c->wh.shower_min);
    add_num(w, "forecast_margin", c->wh.forecast_margin);
    cJSON_AddNumberToObject(w, "start_margin_min", c->wh.start_margin_min);
    cJSON_AddBoolToObject(w, "catchup_hp", c->wh.catchup_hp);
    add_num(w, "solar_on_w", c->wh.solar_on_w);
    cJSON_AddNumberToObject(w, "min_on_min", c->wh.min_on_min);
    cJSON_AddNumberToObject(w, "min_off_min", c->wh.min_off_min);

    add_num(roi, "yield_kwh_kwc", c->roi.yield_kwh_kwc);
    for (int i = 0; i < 12; i++) {
        cJSON_AddItemToArray(mf, cJSON_CreateNumber((double)lroundf(c->roi.month_frac[i] * 100.0f) / 100.0));
    }

    cJSON_AddStringToObject(d, "shelly_ip", c->dev.shelly_ip);
    cJSON_AddStringToObject(d, "envoy_ip", c->dev.envoy_ip);
    add_secret(d, "envoy_token", c->dev.envoy_token);
    cJSON_AddBoolToObject(d, "envoy_fw7", c->dev.envoy_fw7);
    cJSON_AddBoolToObject(d, "envoy_skip_cert", c->dev.envoy_skip_cert);
    cJSON_AddStringToObject(d, "zendure_ip", c->dev.zendure_ip);
    cJSON_AddStringToObject(d, "zendure_sn", c->dev.zendure_sn);
    cJSON_AddStringToObject(d, "tongou_ip", c->dev.tongou_ip);
    static const char *const shelly_modes[] = { "auto", "triphase", "monophase" };
    cJSON_AddStringToObject(d, "shelly_mode", c->shelly.mode <= SHELLY_MODE_MONOPHASE
                                              ? shelly_modes[c->shelly.mode] : "auto");
    cJSON_AddNumberToObject(d, "shelly_channel", c->shelly.channel);
    cJSON_AddStringToObject(d, "tongou_type", c->tuya.type == CE_DEV_TUYA ? "tuya" : "openbeken");
    cJSON_AddStringToObject(d, "tuya_version",
                            c->tuya.version == 35 ? "3.5" : (c->tuya.version == 34 ? "3.4" : "3.3"));
    cJSON_AddStringToObject(d, "tuya_id", c->tuya.dev_id);
    add_secret(d, "tuya_key", c->tuya.local_key);
    cJSON_AddNumberToObject(d, "tuya_dp_switch", c->tuya.dp_switch);
    cJSON_AddNumberToObject(d, "tuya_dp_power", c->tuya.dp_power);
    cJSON_AddNumberToObject(d, "tuya_power_div", c->tuya.power_div);

    add_secret(s, "admin_pass", c->sys.admin_pass);
    cJSON_AddNumberToObject(s, "poll_ms", c->sys.poll_ms);
    cJSON_AddBoolToObject(s, "observer", c->sys.observer);
    cJSON_AddStringToObject(s, "hostname", c->sys.hostname);
    cJSON_AddStringToObject(s, "wifi_ssid", c->sys.wifi_ssid);
    add_secret(s, "wifi_pass", c->sys.wifi_pass);
    return r;
}

/* ---------- Lecture (champ absent = inchangé, mauvais type = erreur) ---------- */

static const cJSON *item(const cJSON *o, const char *k)
{
    return cJSON_GetObjectItemCaseSensitive(o, k);
}

static bool get_float(const cJSON *o, const char *k, float *dst)
{
    const cJSON *it = item(o, k);
    if (it == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(it)) {
        return false;
    }
    *dst = (float)it->valuedouble;
    return true;
}

static bool get_uint(const cJSON *o, const char *k, uint32_t max, uint32_t *dst)
{
    const cJSON *it = item(o, k);
    if (it == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(it) || it->valuedouble < 0 || it->valuedouble > max ||
        it->valuedouble != (double)(uint32_t)it->valuedouble) {
        return false;
    }
    *dst = (uint32_t)it->valuedouble;
    return true;
}

static bool get_u16(const cJSON *o, const char *k, uint16_t *dst)
{
    uint32_t v = *dst;
    if (!get_uint(o, k, UINT16_MAX, &v)) {
        return false;
    }
    *dst = (uint16_t)v;
    return true;
}

static bool get_u8(const cJSON *o, const char *k, uint8_t *dst)
{
    uint32_t v = *dst;
    if (!get_uint(o, k, UINT8_MAX, &v)) {
        return false;
    }
    *dst = (uint8_t)v;
    return true;
}

static bool get_bool(const cJSON *o, const char *k, bool *dst)
{
    const cJSON *it = item(o, k);
    if (it == NULL) {
        return true;
    }
    if (!cJSON_IsBool(it)) {
        return false;
    }
    *dst = cJSON_IsTrue(it);
    return true;
}

bool json_get_str(const cJSON *obj, const char *key, char *dst, size_t cap, bool secret)
{
    const cJSON *it = item(obj, key);
    if (it == NULL) {
        return true;
    }
    if (!cJSON_IsString(it)) {
        return false;
    }
    if (secret && strcmp(it->valuestring, SECRET_MASK) == 0) {
        return true;
    }
    size_t len = strlen(it->valuestring);
    if (len >= cap) {
        return false;
    }
    /* Remise à zéro complète : aucun reste d'une ancienne valeur dans le blob. */
    memset(dst, 0, cap);
    memcpy(dst, it->valuestring, len);
    return true;
}

static bool parse_hhmm(const char *s, uint16_t *out)
{
    if (strlen(s) != 5 || s[2] != ':') {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (i != 2 && (s[i] < '0' || s[i] > '9')) {
            return false;
        }
    }
    unsigned h = (unsigned)(s[0] - '0') * 10 + (unsigned)(s[1] - '0');
    unsigned m = (unsigned)(s[3] - '0') * 10 + (unsigned)(s[4] - '0');
    if (h > 23 || m > 59) {
        return false;
    }
    *out = (uint16_t)(h * 60 + m);
    return true;
}

static bool get_hhmm(const cJSON *o, const char *k, uint16_t *dst)
{
    const cJSON *it = item(o, k);
    if (it == NULL) {
        return true;
    }
    return cJSON_IsString(it) && parse_hhmm(it->valuestring, dst);
}

/* "AAAA-MM-JJ" → AAAAMMJJ, "" → 0. La validité du jour est vérifiée par cfg_validate. */
static bool get_date(const cJSON *o, const char *k, uint32_t *dst)
{
    const cJSON *it = item(o, k);
    if (it == NULL) {
        return true;
    }
    if (!cJSON_IsString(it)) {
        return false;
    }
    const char *s = it->valuestring;
    if (s[0] == '\0') {
        *dst = 0;
        return true;
    }
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-') {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *dst = v;
    return true;
}

static const char *tariff_from_json(const cJSON *o, cfg_tariff_t *t)
{
    const cJSON *type = item(o, "type");
    if (type != NULL) {
        TRY(cJSON_IsString(type), "tariff.type");
        if (strcmp(type->valuestring, "base") == 0) {
            t->type = TARIFF_BASE;
        } else if (strcmp(type->valuestring, "hphc") == 0) {
            t->type = TARIFF_HPHC;
        } else {
            return "tariff.type";
        }
    }

    const cJSON *hc = item(o, "hc");
    if (hc != NULL) {
        TRY(cJSON_IsArray(hc) && cJSON_GetArraySize(hc) <= CFG_HC_SLOTS_MAX, "tariff.hc");
        uint8_t n = 0;
        const cJSON *sl;
        cJSON_ArrayForEach(sl, hc) {
            /* Format "HH:MM-HH:MM" */
            TRY(cJSON_IsString(sl) && strlen(sl->valuestring) == 11 && sl->valuestring[5] == '-',
                "tariff.hc");
            char a[6], b[6];
            memcpy(a, sl->valuestring, 5);
            a[5] = '\0';
            memcpy(b, sl->valuestring + 6, 5);
            b[5] = '\0';
            TRY(parse_hhmm(a, &t->hc[n].start_min) && parse_hhmm(b, &t->hc[n].end_min), "tariff.hc");
            n++;
        }
        for (int i = n; i < CFG_HC_SLOTS_MAX; i++) {
            t->hc[i].start_min = 0;
            t->hc[i].end_min = 0;
        }
        t->n_hc = n;
    }

    TRY(get_float(o, "price_hp", &t->price_hp), "tariff.price_hp");
    TRY(get_float(o, "price_hc", &t->price_hc), "tariff.price_hc");
    TRY(get_float(o, "price_base", &t->price_base), "tariff.price_base");
    TRY(get_float(o, "price_sell", &t->price_sell), "tariff.price_sell");
    /* Toute écriture de la section tarif vaut saisie par l'utilisateur. */
    t->configured = true;
    return NULL;
}

static const char *pv_from_json(const cJSON *o, cfg_pv_t *p)
{
    TRY(get_float(o, "kwc", &p->kwc), "pv.kwc");
    TRY(get_float(o, "lat", &p->lat), "pv.lat");
    TRY(get_float(o, "lon", &p->lon), "pv.lon");
    TRY(get_float(o, "tilt", &p->tilt), "pv.tilt");
    TRY(get_float(o, "azimuth", &p->azimuth), "pv.azimuth");
    return NULL;
}

static const char *batt_from_json(const cJSON *o, cfg_batt_t *b)
{
    TRY(get_float(o, "capa_kwh", &b->capa_kwh), "batt.capa_kwh");
    TRY(get_float(o, "p_charge_max", &b->p_charge_max), "batt.p_charge_max");
    TRY(get_float(o, "p_discharge_max", &b->p_discharge_max), "batt.p_discharge_max");
    TRY(get_float(o, "eta", &b->eta), "batt.eta");
    TRY(get_u8(o, "soc_min", &b->soc_min), "batt.soc_min");
    TRY(get_u8(o, "soc_max", &b->soc_max), "batt.soc_max");
    TRY(get_float(o, "price_eur", &b->price_eur), "batt.price_eur");
    TRY(get_u16(o, "cycles", &b->cycles), "batt.cycles");
    TRY(get_date(o, "commissioning", &b->commissioning), "batt.commissioning");
    TRY(get_bool(o, "arbitrage", &b->arbitrage), "batt.arbitrage");
    return NULL;
}

static const char *wh_from_json(const cJSON *o, cfg_wh_t *w)
{
    TRY(get_float(o, "power_w", &w->power_w), "wh.power_w");
    TRY(get_u16(o, "full_heat_min", &w->full_heat_min), "wh.full_heat_min");
    TRY(get_hhmm(o, "shower", &w->shower_min), "wh.shower");
    TRY(get_float(o, "forecast_margin", &w->forecast_margin), "wh.forecast_margin");
    TRY(get_u16(o, "start_margin_min", &w->start_margin_min), "wh.start_margin_min");
    TRY(get_bool(o, "catchup_hp", &w->catchup_hp), "wh.catchup_hp");
    TRY(get_float(o, "solar_on_w", &w->solar_on_w), "wh.solar_on_w");
    TRY(get_u16(o, "min_on_min", &w->min_on_min), "wh.min_on_min");
    TRY(get_u16(o, "min_off_min", &w->min_off_min), "wh.min_off_min");
    return NULL;
}

static const char *roi_from_json(const cJSON *o, cfg_roi_t *r)
{
    TRY(get_float(o, "yield_kwh_kwc", &r->yield_kwh_kwc), "roi.yield_kwh_kwc");
    const cJSON *mf = item(o, "month_frac");
    if (mf != NULL) {
        TRY(cJSON_IsArray(mf) && cJSON_GetArraySize(mf) == 12, "roi.month_frac");
        int i = 0;
        const cJSON *v;
        cJSON_ArrayForEach(v, mf) {
            TRY(cJSON_IsNumber(v), "roi.month_frac");
            r->month_frac[i++] = (float)v->valuedouble;
        }
    }
    return NULL;
}

static const char *dev_from_json(const cJSON *o, cfg_dev_t *d)
{
    TRY(json_get_str(o, "shelly_ip", d->shelly_ip, sizeof(d->shelly_ip), false), "dev.shelly_ip");
    TRY(json_get_str(o, "envoy_ip", d->envoy_ip, sizeof(d->envoy_ip), false), "dev.envoy_ip");
    TRY(json_get_str(o, "envoy_token", d->envoy_token, sizeof(d->envoy_token), true), "dev.envoy_token");
    TRY(get_bool(o, "envoy_fw7", &d->envoy_fw7), "dev.envoy_fw7");
    TRY(get_bool(o, "envoy_skip_cert", &d->envoy_skip_cert), "dev.envoy_skip_cert");
    TRY(json_get_str(o, "zendure_ip", d->zendure_ip, sizeof(d->zendure_ip), false), "dev.zendure_ip");
    TRY(json_get_str(o, "zendure_sn", d->zendure_sn, sizeof(d->zendure_sn), false), "dev.zendure_sn");
    TRY(json_get_str(o, "tongou_ip", d->tongou_ip, sizeof(d->tongou_ip), false), "dev.tongou_ip");
    return NULL;
}

/* Champs v2 du Shelly, exposés dans la section "dev". */
static const char *shelly_from_json(const cJSON *o, cfg_shelly_t *s)
{
    const cJSON *mode = item(o, "shelly_mode");
    if (mode != NULL) {
        TRY(cJSON_IsString(mode), "dev.shelly_mode");
        if (strcmp(mode->valuestring, "auto") == 0) {
            s->mode = SHELLY_MODE_AUTO;
        } else if (strcmp(mode->valuestring, "triphase") == 0) {
            s->mode = SHELLY_MODE_TRIPHASE;
        } else if (strcmp(mode->valuestring, "monophase") == 0) {
            s->mode = SHELLY_MODE_MONOPHASE;
        } else {
            return "dev.shelly_mode";
        }
    }
    TRY(get_u8(o, "shelly_channel", &s->channel), "dev.shelly_channel");
    return NULL;
}

/* Champs v3 de l'interrupteur Tuya, exposés dans la section "dev". */
static const char *tuya_from_json(const cJSON *o, cfg_tuya_t *t)
{
    const cJSON *type = item(o, "tongou_type");
    if (type != NULL) {
        TRY(cJSON_IsString(type), "dev.tongou_type");
        if (strcmp(type->valuestring, "openbeken") == 0) {
            t->type = CE_DEV_OPENBEKEN;
        } else if (strcmp(type->valuestring, "tuya") == 0) {
            t->type = CE_DEV_TUYA;
        } else {
            return "dev.tongou_type";
        }
    }
    const cJSON *ver = item(o, "tuya_version");
    if (ver != NULL) {
        TRY(cJSON_IsString(ver), "dev.tuya_version");
        if (strcmp(ver->valuestring, "3.3") == 0) {
            t->version = 33;
        } else if (strcmp(ver->valuestring, "3.4") == 0) {
            t->version = 34;
        } else if (strcmp(ver->valuestring, "3.5") == 0) {
            t->version = 35;
        } else {
            return "dev.tuya_version";
        }
    }
    TRY(json_get_str(o, "tuya_id", t->dev_id, sizeof(t->dev_id), false), "dev.tuya_id");
    TRY(json_get_str(o, "tuya_key", t->local_key, sizeof(t->local_key), true), "dev.tuya_key");
    TRY(get_u8(o, "tuya_dp_switch", &t->dp_switch), "dev.tuya_dp_switch");
    TRY(get_u8(o, "tuya_dp_power", &t->dp_power), "dev.tuya_dp_power");
    TRY(get_u16(o, "tuya_power_div", &t->power_div), "dev.tuya_power_div");
    return NULL;
}

static const char *sys_from_json(const cJSON *o, cfg_sys_t *s)
{
    TRY(json_get_str(o, "admin_pass", s->admin_pass, sizeof(s->admin_pass), true), "sys.admin_pass");
    TRY(get_u16(o, "poll_ms", &s->poll_ms), "sys.poll_ms");
    TRY(get_bool(o, "observer", &s->observer), "sys.observer");
    TRY(json_get_str(o, "hostname", s->hostname, sizeof(s->hostname), false), "sys.hostname");
    TRY(json_get_str(o, "wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid), false), "sys.wifi_ssid");
    TRY(json_get_str(o, "wifi_pass", s->wifi_pass, sizeof(s->wifi_pass), true), "sys.wifi_pass");
    return NULL;
}

/* Section absente : rien à faire ; présente mais pas un objet : erreur sur la section. */
#define SECTION(key, fn, dst) do {                              \
        const cJSON *o_ = item(root, key);                      \
        if (o_ != NULL) {                                       \
            TRY(cJSON_IsObject(o_), key);                       \
            const char *bad_ = fn(o_, dst);                     \
            if (bad_ != NULL) {                                 \
                return bad_;                                    \
            }                                                   \
        }                                                       \
    } while (0)

const char *cfg_from_json(const cJSON *root, cfg_t *c)
{
    SECTION("tariff", tariff_from_json, &c->tariff);
    SECTION("pv", pv_from_json, &c->pv);
    SECTION("batt", batt_from_json, &c->batt);
    SECTION("wh", wh_from_json, &c->wh);
    SECTION("roi", roi_from_json, &c->roi);
    SECTION("dev", dev_from_json, &c->dev);
    SECTION("dev", shelly_from_json, &c->shelly);
    SECTION("dev", tuya_from_json, &c->tuya);
    SECTION("sys", sys_from_json, &c->sys);
    return NULL;
}
