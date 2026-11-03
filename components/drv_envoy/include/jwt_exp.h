#pragma once

/* Lecture de la date d'expiration d'un JWT, sans vérification de signature. C pur (testable sur PC). */

#include <stdint.h>

/* Retourne le champ "exp" (epoch, s) de la charge utile, ou 0 si absent ou illisible. */
int64_t jwt_exp(const char *token);
