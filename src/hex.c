#include "hex.h"

#include <ctype.h>

static int nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int couart_hex_decode(const char *s, uint8_t *out, size_t max, size_t *out_n)
{
    if (!s || !out || !out_n)
        return -1;
    size_t n = 0;
    while (s[0] && s[1]) {
        if (isspace((unsigned char)s[0])) {
            s++;
            continue;
        }
        int hi = nibble((unsigned char)s[0]);
        int lo = nibble((unsigned char)s[1]);
        if (hi < 0 || lo < 0)
            return -1;
        if (n >= max)
            return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    if (s[0] && !isspace((unsigned char)s[0]))
        return -1;
    *out_n = n;
    return 0;
}

int couart_hex_encode(const uint8_t *in, size_t n, char *out, size_t out_n)
{
    static const char *hexd = "0123456789abcdef";
    if (!in || !out)
        return -1;
    if (out_n < n * 2 + 1)
        return -1;
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hexd[in[i] >> 4];
        out[i * 2 + 1] = hexd[in[i] & 0xf];
    }
    out[n * 2] = '\0';
    return 0;
}
