#ifndef COUART_HEX_H
#define COUART_HEX_H

#include <stddef.h>
#include <stdint.h>

int couart_hex_decode(const char *s, uint8_t *out, size_t max, size_t *out_n);
int couart_hex_encode(const uint8_t *in, size_t n, char *out, size_t out_n);

#endif
