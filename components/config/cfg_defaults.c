#include <string.h>
#include "cfg_types.h"

/* TODO(verify) : puissances max de charge et de décharge du SolarFlow 1600 AC+ à relever
 * sur la fiche technique. Valeurs prudentes en attendant. */
#define DEF_BATT_P_CHARGE_MAX_W     800.0f
#define DEF_BATT_P_DISCHARGE_MAX_W  800.0f

/* Répartition mensuelle approximative de la production (§9.9), TODO(verify) selon le site. */
static const float k_month_frac[12] = {
    3.0f, 5.0f, 8.0f, 10.5f, 12.0f, 13.0f, 13.5f, 12.0f, 9.0f, 6.5f, 4.0f, 3.5f
};

void cfg_set_defaults(cfg_t *c)
{
    memset(c, 0, sizeof(*c));

    /* Tarif : pas de valeur par défaut fictive, saisie obligatoire (§11.6). */
    c->tariff.type = TARIFF_HPHC;
    c->tariff.configured = false;

    /* Installation PV : à saisir (lat/lon à 0 = non renseignées). */
    c->pv.kwc = 1.0f;
    c->pv.tilt = 30.0f;
    c->pv.azimuth = 0.0f;

    c->batt.capa_kwh = 3.2f;
    c->batt.p_charge_max = DEF_BATT_P_CHARGE_MAX_W;
    c->batt.p_discharge_max = DEF_BATT_P_DISCHARGE_MAX_W;
    c->batt.eta = 0.88f;
    c->batt.soc_min = 10;
    c->batt.soc_max = 100;
    c->batt.price_eur = 0.0f;       /* à saisir : coût d'usure nul tant qu'il manque */
    c->batt.cycles = 6000;          /* indicatif, à remplacer par la garantie du fabricant */
    c->batt.commissioning = 0;
    c->batt.arbitrage = false;

    c->wh.power_w = 2500.0f;
    c->wh.full_heat_min = 105;
    c->wh.shower_min = 20 * 60;
    c->wh.forecast_margin = 0.7f;
    c->wh.start_margin_min = 15;
    c->wh.catchup_hp = true;
    c->wh.solar_on_w = 1500.0f;
    c->wh.min_on_min = 10;
    c->wh.min_off_min = 5;

    c->roi.yield_kwh_kwc = 1200.0f;
    memcpy(c->roi.month_frac, k_month_frac, sizeof(k_month_frac));

    c->dev.envoy_fw7 = true;
    c->dev.envoy_skip_cert = true;

    c->shelly.mode = SHELLY_MODE_AUTO;
    c->shelly.channel = 0;

    /* TODO(verify) : DP courants des prises Tuya avec mesure (1 = relais, 19 = puissance en 0,1 W),
     * à confirmer sur l'appareil avec « python tools/tuya_ref.py probe ». */
    /* Saison haute par défaut de novembre à mars (TURPE, délibération CRE du 28/06/2018). Prix à saisir. */
    c->grid.high_months = (1u << 10) | (1u << 11) | (1u << 0) | (1u << 1) | (1u << 2);
    c->grid.holidays_weekend = 1;

    c->tuya.type = CE_DEV_OPENBEKEN;
    c->tuya.version = 33;
    c->tuya.dp_switch = 1;
    c->tuya.dp_power = 19;
    c->tuya.power_div = 10;

    c->sys.poll_ms = 2000;
    c->sys.observer = true;         /* aucune commande tant que l'utilisateur ne l'a pas décidé */
    strcpy(c->sys.hostname, "solaire");
}
