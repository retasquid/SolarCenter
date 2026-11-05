#pragma once

/* Types de configuration : C pur (aucune dépendance ESP-IDF), testable sur PC. */

#include <stdint.h>
#include <stdbool.h>

#define CFG_VERSION         4
#define CFG_V3_DATA         1154    /* octets utiles de cfg_t en v3 (fin de cfg_tuya_t) */
#define CFG_V1_SIZE         1104    /* taille de cfg_t en v1 : les champs v2 sont ajoutés après */
#define CFG_V2_DATA         1106    /* octets utiles de cfg_t en v2 (fin de cfg_shelly_t) */
#define CFG_TUYA_ID_LEN     24
#define CFG_TUYA_KEY_LEN    17      /* clé locale : 16 caractères + NUL */

#define CFG_HC_SLOTS_MAX    4
#define CFG_IP_LEN          16      /* "255.255.255.255" + NUL */
#define CFG_SN_LEN          32
#define CFG_TOKEN_LEN       640     /* jeton JWT Envoy, avec marge */
#define CFG_SSID_LEN        33      /* 32 octets + NUL */
#define CFG_WIFI_PASS_LEN   64      /* 63 caractères + NUL */
#define CFG_ADMIN_PASS_LEN  65      /* 64 caractères + NUL */
#define CFG_HOSTNAME_LEN    32
#define CFG_MINUTES_PER_DAY 1440

typedef enum {
    TARIFF_BASE = 0,
    TARIFF_HPHC = 1,
    TARIFF_GRID = 2,        /* HP/HC × semaine/week-end × saison basse/haute (v4) */
} tariff_type_t;

/* Plage horaire [start, end[ en minutes depuis minuit ; end < start = passe minuit. */
typedef struct {
    uint16_t start_min;
    uint16_t end_min;
} cfg_slot_t;

typedef struct {
    uint8_t    type;            /* tariff_type_t */
    bool       configured;      /* saisi par l'utilisateur : aucune valeur fictive par défaut */
    uint8_t    n_hc;
    cfg_slot_t hc[CFG_HC_SLOTS_MAX];
    float      price_hp;        /* €/kWh */
    float      price_hc;
    float      price_base;
    float      price_sell;
} cfg_tariff_t;

typedef struct {
    float kwc;
    float lat;
    float lon;
    float tilt;                 /* ° */
    float azimuth;              /* ° — TODO(verify) : convention d'Open-Meteo (0 = sud ?) */
} cfg_pv_t;

typedef struct {
    float    capa_kwh;          /* capacité utile */
    float    p_charge_max;      /* W */
    float    p_discharge_max;   /* W */
    float    eta;               /* rendement aller-retour */
    uint8_t  soc_min;           /* % */
    uint8_t  soc_max;           /* % */
    float    price_eur;
    uint16_t cycles;            /* cycles garantis */
    uint32_t commissioning;     /* AAAAMMJJ, 0 = non renseignée */
    bool     arbitrage;
} cfg_batt_t;

typedef struct {
    float    power_w;
    uint16_t full_heat_min;     /* durée de chauffe complète initiale */
    uint16_t shower_min;        /* heure des douches, minutes depuis minuit */
    float    forecast_margin;
    uint16_t start_margin_min;
    bool     catchup_hp;        /* rattrapage en HP */
    float    solar_on_w;        /* seuil de démarrage solaire */
    uint16_t min_on_min;
    uint16_t min_off_min;
} cfg_wh_t;

typedef struct {
    float yield_kwh_kwc;        /* productible, kWh/kWc/an */
    float month_frac[12];       /* % de la production annuelle, total = 100 */
} cfg_roi_t;

typedef struct {
    char shelly_ip[CFG_IP_LEN];
    char envoy_ip[CFG_IP_LEN];
    char envoy_token[CFG_TOKEN_LEN];
    bool envoy_fw7;             /* firmware ≥ 7 : HTTPS + jeton */
    bool envoy_skip_cert;       /* certificat auto-signé : pas de vérification */
    char zendure_ip[CFG_IP_LEN];
    char zendure_sn[CFG_SN_LEN];
    char tongou_ip[CFG_IP_LEN];
} cfg_dev_t;

typedef struct {
    char     admin_pass[CFG_ADMIN_PASS_LEN];
    uint16_t poll_ms;           /* période de sondage du Shelly */
    bool     observer;          /* décisions calculées mais aucune commande envoyée */
    char     hostname[CFG_HOSTNAME_LEN];
    char     wifi_ssid[CFG_SSID_LEN];
    char     wifi_pass[CFG_WIFI_PASS_LEN];
} cfg_sys_t;

typedef enum {
    SHELLY_MODE_AUTO = 0,       /* EM (triphasé), sinon EM1 si l'appareil répond 404 */
    SHELLY_MODE_TRIPHASE = 1,   /* EM.GetStatus */
    SHELLY_MODE_MONOPHASE = 2,  /* EM1.GetStatus sur un canal (profil « monophase » du Pro 3EM) */
} shelly_mode_t;

/* v2. Uniquement des uint8_t : alignement 1, la structure suit la fin de cfg_t v1 sans décalage. */
typedef struct {
    uint8_t mode;               /* shelly_mode_t */
    uint8_t channel;            /* canal EM1 du compteur réseau en monophasé (0–2) */
} cfg_shelly_t;

typedef enum {
    CE_DEV_OPENBEKEN = 0,       /* API HTTP compatible Tasmota (§8.4) */
    CE_DEV_TUYA = 1,            /* firmware Tuya d'origine, protocole local chiffré (choix utilisateur) */
} ce_dev_type_t;

/* v3. Interrupteur du chauffe-eau sous firmware Tuya. */
typedef struct {
    uint8_t  type;              /* ce_dev_type_t */
    uint8_t  version;           /* 33 ou 34 (protocole 3.3 / 3.4) */
    uint8_t  dp_switch;         /* DP du relais (booléen) */
    uint8_t  dp_power;          /* DP de la puissance */
    uint16_t power_div;         /* W = valeur brute / power_div */
    char     dev_id[CFG_TUYA_ID_LEN];
    char     local_key[CFG_TUYA_KEY_LEN];
} cfg_tuya_t;

/* v4. Grille de prix (tarif TARIFF_GRID), plages HC de cfg_tariff_t appliquées tous les jours. */
typedef struct {
    uint16_t high_months;       /* bit (m − 1) = mois m en saison haute */
    uint8_t  holidays_weekend;  /* jours fériés au prix du week-end */
    uint8_t  reserved;
    float    price[2][2][2];    /* €/kWh TTC [saison basse/haute][semaine/week-end][HP/HC] */
} cfg_grid_t;

typedef struct {
    cfg_tariff_t tariff;
    cfg_pv_t     pv;
    cfg_batt_t   batt;
    cfg_wh_t     wh;
    cfg_roi_t    roi;
    cfg_dev_t    dev;
    cfg_sys_t    sys;
    /* ---- ajouts en fin de structure uniquement (migration par recopie du préfixe connu) ---- */
    cfg_shelly_t shelly;        /* v2 */
    cfg_tuya_t   tuya;          /* v3 */
    cfg_grid_t   grid;          /* v4 */
} cfg_t;

void cfg_set_defaults(cfg_t *c);

/* Retourne NULL si la config est valide, sinon le champ fautif (ex. "tariff.price_hp"). */
const char *cfg_validate(const cfg_t *c);

bool cfg_is_ipv4(const char *s);
