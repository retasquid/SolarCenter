#pragma once

/* Tarif base ou HP/HC (CLAUDE.md §9.1). C pur.
 * Les plages HC sont en heure locale : l'appelant passe la minute locale du jour (0–1439),
 * ce qui règle les changements d'heure été/hiver en amont (localtime). */

#include <stdbool.h>
#include <stdint.h>

#define TARIFF_SLOTS_MAX 4

typedef struct {
    bool    hphc;               /* faux : tarif base */
    uint8_t n_hc;
    struct {
        uint16_t start_min;     /* [start, end[ ; end < start = passe minuit ; end = 0 = jusqu'à minuit */
        uint16_t end_min;
    } hc[TARIFF_SLOTS_MAX];
    float   price_hp;           /* €/kWh */
    float   price_hc;
    float   price_base;
    float   price_sell;
} tariff_t;

bool tariff_is_hc(const tariff_t *t, uint16_t minute);

/* Prix d'achat en vigueur. */
float tariff_price(const tariff_t *t, uint16_t minute);

/* Minutes avant le prochain changement HP↔HC (1–1440), 0 s'il n'y en a jamais. */
uint16_t tariff_next_change(const tariff_t *t, uint16_t minute, bool *next_is_hc);
