#pragma once

/* Profil de consommation de base : EMA sur 48 créneaux de 30 min × 2 types de jour (§9.2 b). C pur. */

#include <stdbool.h>
#include <stdint.h>

#define PROFILE_SLOTS       48
#define PROFILE_DAY_TYPES   2       /* 0 = semaine, 1 = week-end */
#define PROFILE_ALPHA       0.1f    /* prof = 0,9·prof + 0,1·mesure */
#define PROFILE_DEFAULT_WH  300.0f  /* repli si le créneau n'a jamais été mesuré */

typedef struct {
    float    wh[PROFILE_DAY_TYPES][PROFILE_SLOTS];
    uint16_t n[PROFILE_DAY_TYPES][PROFILE_SLOTS];   /* nombre de mesures (saturé) */
} profile_t;

void profile_init(profile_t *p);

/* weekday : 0 = dimanche … 6 = samedi. */
int profile_day_type(int weekday);

/* Ignore les valeurs non finies ou négatives. Première mesure d'un créneau : prise telle quelle. */
bool profile_update(profile_t *p, int day_type, int slot, float wh);

/* Valeur du profil, ou PROFILE_DEFAULT_WH si le créneau est vide. */
float profile_get(const profile_t *p, int day_type, int slot);
