#include <string.h>
#include "jwt_exp.h"

#define JWT_PAYLOAD_MAX 512     /* charge utile décodée */

static int b64url_val(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '-') {
        return 62;
    }
    if (ch == '_') {
        return 63;
    }
    return -1;
}

/* Décode [src, src+n[ en base64url sans remplissage. Retourne la longueur ou -1. */
static int b64url_decode(const char *src, int n, char *dst, int cap)
{
    unsigned acc = 0;
    int bits = 0;
    int out = 0;
    for (int i = 0; i < n; i++) {
        int v = b64url_val(src[i]);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out >= cap) {
                return -1;
            }
            dst[out++] = (char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

int64_t jwt_exp(const char *token)
{
    const char *p1 = strchr(token, '.');
    if (p1 == NULL) {
        return 0;
    }
    const char *p2 = strchr(p1 + 1, '.');
    if (p2 == NULL) {
        return 0;
    }

    char payload[JWT_PAYLOAD_MAX + 1];
    int n = b64url_decode(p1 + 1, (int)(p2 - p1 - 1), payload, JWT_PAYLOAD_MAX);
    if (n <= 0) {
        return 0;
    }
    payload[n] = '\0';

    /* Recherche de "exp" : valeur numérique entière. */
    const char *k = strstr(payload, "\"exp\"");
    if (k == NULL) {
        return 0;
    }
    k += 5;
    while (*k == ' ' || *k == '\t') {
        k++;
    }
    if (*k++ != ':') {
        return 0;
    }
    while (*k == ' ' || *k == '\t') {
        k++;
    }
    int64_t v = 0;
    int digits = 0;
    while (*k >= '0' && *k <= '9' && digits < 12) {
        v = v * 10 + (*k - '0');
        k++;
        digits++;
    }
    return digits > 0 ? v : 0;
}
