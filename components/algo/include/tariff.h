#pragma once

/* Tarif (CLAUDE.md §9.1). C pur.
 * - base : un prix unique ;
 * - HP/HC : plages HC quotidiennes, prix HP et HC ;
 * - grille : HP/HC × semaine/week-end × saison basse/haute (ex. option Week-end avec acheminement
 *   saisonnier). Les plages HC (celles d'Enedis) s'appliquent tous les jours.
 * Les plages sont en heure locale : l'appelant passe la date et la minute locales (localtime),
 * ce qui règle les changements d'heure en amont. */

#include <stdbool.h>
#include <stdint.h>

#define TARIFF_SLOTS_MAX 4

/* Indices de la grille de prix. */
#define TARIFF_LOW_SEASON   0
#define TARIFF_HIGH_SEASON  1
#define TARIFF_WEEKDAY      0
#define TARIFF_WEEKEND      1
#define TARIFF_HP           0
#define TARIFF_HC           1

typedef struct {
    bool    hphc;               /* faux : tarif base */
    bool    grid;               /* vrai : prix de la grille (hphc vrai aussi) */
    uint8_t n_hc;
    struct {
        uint16_t start_min;     /* [start, end[ ; end < start = passe minuit ; end = 0 = jusqu'à minuit */
        uint16_t end_min;
    } hc[TARIFF_SLOTS_MAX];
    float    price_hp;          /* €/kWh */
    float    price_hc;
    float    price_base;
    float    price_sell;
    uint16_t high_months;       /* grille : bit (m − 1) = mois m en saison haute */
    bool     holidays_weekend;  /* grille : jours fériés au prix du week-end */
    float    grid_price[2][2][2];   /* [saison][type de jour][HP/HC] */
} tariff_t;

/* Jour local. wday : 0 = dimanche … 6 = samedi. */
typedef struct {
    int16_t year;
    uint8_t month;              /* 1–12 */
    uint8_t mday;               /* 1–31 */
    uint8_t wday;
} tariff_day_t;

/* Jours fériés de France métropolitaine (Pâques par l'algorithme grégorien anonyme). */
bool tariff_is_holiday(int year, int month, int mday);

bool tariff_is_weekend(const tariff_t *t, const tariff_day_t *d);
bool tariff_high_season(const tariff_t *t, int month);

bool tariff_is_hc(const tariff_t *t, uint16_t minute);

/* Prix d'achat en vigueur. */
float tariff_price_at(const tariff_t *t, const tariff_day_t *d, uint16_t minute);

/* Minutes avant le prochain changement HP↔HC des plages (1–1440), 0 s'il n'y en a jamais. */
/* Prix moyen HP (hc = false) ou HC sur l'année, pondéré semaine / week-end et saisons (projections). */
float tariff_mean_price(const tariff_t *t, bool hc);

uint16_t tariff_next_change(const tariff_t *t, uint16_t minute, bool *next_is_hc);
