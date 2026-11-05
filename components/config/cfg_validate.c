#include <string.h>
#include <math.h>
#include "cfg_types.h"

#define TRY(cond, field) do { if (!(cond)) { return (field); } } while (0)

static bool in_range(float v, float lo, float hi)
{
    return isfinite(v) && v >= lo && v <= hi;
}

static bool terminated(const char *s, unsigned cap)
{
    return memchr(s, '\0', cap) != NULL;
}

static bool chars_in(const char *s, bool (*ok)(char))
{
    for (; *s != '\0'; s++) {
        if (!ok(*s)) {
            return false;
        }
    }
    return true;
}

static bool is_printable(char ch)
{
    return ch >= 0x20 && ch <= 0x7E;
}

static bool is_graph(char ch)
{
    return ch > 0x20 && ch <= 0x7E;
}

static bool is_jwt_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.';
}

static bool is_host_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
}

bool cfg_is_ipv4(const char *s)
{
    for (int part = 0; part < 4; part++) {
        int val = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (++digits > 3 || val > 255) {
                return false;
            }
            s++;
        }
        if (digits == 0) {
            return false;
        }
        if (part < 3) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    return *s == '\0';
}

/* Adresse vide (appareil absent) ou IPv4 valide. */
static bool ip_ok(const char *s, unsigned cap)
{
    return terminated(s, cap) && (s[0] == '\0' || cfg_is_ipv4(s));
}

/* Mot de passe vide ou de longueur [min, cap-1], caractères imprimables. */
static bool pass_ok(const char *s, unsigned cap, unsigned min)
{
    if (!terminated(s, cap)) {
        return false;
    }
    size_t len = strlen(s);
    return (len == 0 || len >= min) && chars_in(s, is_printable);
}

static bool slot_ok(const cfg_slot_t *s)
{
    return s->start_min < CFG_MINUTES_PER_DAY && s->end_min < CFG_MINUTES_PER_DAY &&
           s->start_min != s->end_min;
}

/* Découpe une plage en au plus deux intervalles [a, b[ dans [0, 1440[. */
static int slot_split(const cfg_slot_t *s, uint16_t a[2], uint16_t b[2])
{
    a[0] = s->start_min;
    if (s->start_min < s->end_min) {
        b[0] = s->end_min;
        return 1;
    }
    b[0] = CFG_MINUTES_PER_DAY;
    if (s->end_min == 0) {
        return 1;
    }
    a[1] = 0;
    b[1] = s->end_min;
    return 2;
}

static bool slots_overlap(const cfg_slot_t *x, const cfg_slot_t *y)
{
    uint16_t xa[2], xb[2], ya[2], yb[2];
    int nx = slot_split(x, xa, xb);
    int ny = slot_split(y, ya, yb);
    for (int i = 0; i < nx; i++) {
        for (int j = 0; j < ny; j++) {
            if (xa[i] < yb[j] && ya[j] < xb[i]) {
                return true;
            }
        }
    }
    return false;
}

static bool date_ok(uint32_t d)
{
    if (d == 0) {
        return true;
    }
    static const uint8_t mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint32_t y = d / 10000;
    uint32_t m = (d / 100) % 100;
    uint32_t day = d % 100;
    if (y < 2000 || y > 2100 || m < 1 || m > 12 || day < 1 || day > mdays[m - 1]) {
        return false;
    }
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return !(m == 2 && day == 29 && !leap);
}

static const char *validate_tariff(const cfg_tariff_t *t)
{
    TRY(t->type == TARIFF_BASE || t->type == TARIFF_HPHC, "tariff.type");
    TRY(t->n_hc <= CFG_HC_SLOTS_MAX, "tariff.hc");
    TRY(!(t->configured && t->type == TARIFF_HPHC && t->n_hc == 0), "tariff.hc");
    for (int i = 0; i < t->n_hc; i++) {
        TRY(slot_ok(&t->hc[i]), "tariff.hc");
        for (int j = 0; j < i; j++) {
            TRY(!slots_overlap(&t->hc[i], &t->hc[j]), "tariff.hc");
        }
    }
    TRY(in_range(t->price_hp, 0.0f, 2.0f), "tariff.price_hp");
    TRY(in_range(t->price_hc, 0.0f, 2.0f), "tariff.price_hc");
    TRY(in_range(t->price_base, 0.0f, 2.0f), "tariff.price_base");
    TRY(in_range(t->price_sell, 0.0f, 1.0f), "tariff.price_sell");
    return NULL;
}

static const char *validate_pv(const cfg_pv_t *p)
{
    TRY(in_range(p->kwc, 0.1f, 36.0f), "pv.kwc");
    TRY(in_range(p->lat, -90.0f, 90.0f), "pv.lat");
    TRY(in_range(p->lon, -180.0f, 180.0f), "pv.lon");
    TRY(in_range(p->tilt, 0.0f, 90.0f), "pv.tilt");
    TRY(in_range(p->azimuth, -180.0f, 180.0f), "pv.azimuth");
    return NULL;
}

static const char *validate_batt(const cfg_batt_t *b)
{
    TRY(in_range(b->capa_kwh, 0.1f, 50.0f), "batt.capa_kwh");
    /* Borne de plausibilité |p_batt| < 3000 W (§7). */
    TRY(in_range(b->p_charge_max, 0.0f, 2999.0f), "batt.p_charge_max");
    TRY(in_range(b->p_discharge_max, 0.0f, 2999.0f), "batt.p_discharge_max");
    TRY(in_range(b->eta, 0.5f, 1.0f), "batt.eta");
    TRY(b->soc_max <= 100, "batt.soc_max");
    TRY(b->soc_min < b->soc_max, "batt.soc_min");
    TRY(in_range(b->price_eur, 0.0f, 20000.0f), "batt.price_eur");
    TRY(b->cycles >= 1, "batt.cycles");
    TRY(date_ok(b->commissioning), "batt.commissioning");
    return NULL;
}

static const char *validate_wh(const cfg_wh_t *w)
{
    /* Borne de plausibilité p_ce < 4000 W (§7). */
    TRY(in_range(w->power_w, 100.0f, 3999.0f), "wh.power_w");
    TRY(w->full_heat_min >= 10 && w->full_heat_min <= 600, "wh.full_heat_min");
    TRY(w->shower_min < CFG_MINUTES_PER_DAY, "wh.shower_min");
    TRY(in_range(w->forecast_margin, 0.3f, 1.0f), "wh.forecast_margin");
    TRY(w->start_margin_min <= 120, "wh.start_margin_min");
    TRY(in_range(w->solar_on_w, 100.0f, 3999.0f), "wh.solar_on_w");
    TRY(w->min_on_min >= 1 && w->min_on_min <= 120, "wh.min_on_min");
    TRY(w->min_off_min >= 1 && w->min_off_min <= 120, "wh.min_off_min");
    return NULL;
}

static const char *validate_roi(const cfg_roi_t *r)
{
    TRY(in_range(r->yield_kwh_kwc, 100.0f, 3000.0f), "roi.yield_kwh_kwc");
    float sum = 0.0f;
    for (int i = 0; i < 12; i++) {
        TRY(in_range(r->month_frac[i], 0.0f, 100.0f), "roi.month_frac");
        sum += r->month_frac[i];
    }
    TRY(fabsf(sum - 100.0f) <= 0.1f, "roi.month_frac");
    return NULL;
}

static const char *validate_dev(const cfg_dev_t *d)
{
    TRY(ip_ok(d->shelly_ip, sizeof(d->shelly_ip)), "dev.shelly_ip");
    TRY(ip_ok(d->envoy_ip, sizeof(d->envoy_ip)), "dev.envoy_ip");
    TRY(terminated(d->envoy_token, sizeof(d->envoy_token)) && chars_in(d->envoy_token, is_jwt_char),
        "dev.envoy_token");
    TRY(ip_ok(d->zendure_ip, sizeof(d->zendure_ip)), "dev.zendure_ip");
    TRY(terminated(d->zendure_sn, sizeof(d->zendure_sn)) && chars_in(d->zendure_sn, is_graph),
        "dev.zendure_sn");
    TRY(ip_ok(d->tongou_ip, sizeof(d->tongou_ip)), "dev.tongou_ip");
    return NULL;
}

static const char *validate_sys(const cfg_sys_t *s)
{
    TRY(pass_ok(s->admin_pass, sizeof(s->admin_pass), 8), "sys.admin_pass");
    TRY(s->poll_ms >= 1000 && s->poll_ms <= 10000, "sys.poll_ms");
    TRY(terminated(s->hostname, sizeof(s->hostname)), "sys.hostname");
    size_t hl = strlen(s->hostname);
    TRY(hl >= 1 && chars_in(s->hostname, is_host_char) &&
        s->hostname[0] != '-' && s->hostname[hl - 1] != '-', "sys.hostname");
    TRY(terminated(s->wifi_ssid, sizeof(s->wifi_ssid)), "sys.wifi_ssid");
    TRY(pass_ok(s->wifi_pass, sizeof(s->wifi_pass), 8), "sys.wifi_pass");
    return NULL;
}

static const char *validate_shelly(const cfg_shelly_t *s)
{
    TRY(s->mode <= SHELLY_MODE_MONOPHASE, "dev.shelly_mode");
    TRY(s->channel <= 2, "dev.shelly_channel");
    return NULL;
}

static bool is_alnum(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
}

static const char *validate_tuya(const cfg_tuya_t *t)
{
    TRY(t->type <= CE_DEV_TUYA, "dev.tongou_type");
    TRY(t->version == 33 || t->version == 34 || t->version == 35, "dev.tuya_version");
    TRY(t->dp_switch >= 1, "dev.tuya_dp_switch");
    TRY(t->dp_power >= 1, "dev.tuya_dp_power");
    TRY(t->power_div >= 1 && t->power_div <= 1000, "dev.tuya_power_div");
    TRY(terminated(t->dev_id, sizeof(t->dev_id)) && chars_in(t->dev_id, is_alnum), "dev.tuya_id");
    TRY(terminated(t->local_key, sizeof(t->local_key)) && chars_in(t->local_key, is_printable) &&
        (t->local_key[0] == '\0' || strlen(t->local_key) == 16), "dev.tuya_key");
    return NULL;
}

const char *cfg_validate(const cfg_t *c)
{
    const char *bad;
    if ((bad = validate_shelly(&c->shelly)) != NULL) {
        return bad;
    }
    if ((bad = validate_tuya(&c->tuya)) != NULL) {
        return bad;
    }
    if ((bad = validate_tariff(&c->tariff)) != NULL) {
        return bad;
    }
    if ((bad = validate_pv(&c->pv)) != NULL) {
        return bad;
    }
    if ((bad = validate_batt(&c->batt)) != NULL) {
        return bad;
    }
    if ((bad = validate_wh(&c->wh)) != NULL) {
        return bad;
    }
    if ((bad = validate_roi(&c->roi)) != NULL) {
        return bad;
    }
    if ((bad = validate_dev(&c->dev)) != NULL) {
        return bad;
    }
    return validate_sys(&c->sys);
}
