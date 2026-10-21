/*
 * Text formatting and byte helpers shared by the added network screens
 * (link_status.c, switch_port.c). No libc on the tester side, so these stand in
 * for the few routines needed.
 */
#ifndef LPM_PATCH_FMT_H
#define LPM_PATCH_FMT_H

#include <stdint.h>

static inline char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static inline char *put_u(char *p, unsigned v)
{
    char digits[10];
    int n = 0;
    do
        digits[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        *p++ = digits[--n];
    *p = 0;
    return p;
}

static inline char *put_ip(char *p, const uint8_t *ip)
{
    for (int i = 0; i < 4; i++) {
        if (i)
            *p++ = '.';
        p = put_u(p, ip[i]);
    }
    return p;
}

static inline char *put_hex(char *p, unsigned v, int digits, const char *hex)
{
    while (digits--)
        *p++ = hex[v >> (4 * digits) & 15];
    *p = 0;
    return p;
}

static inline char *put_mac(char *p, const uint8_t *mac)
{
    for (int i = 0; i < 6; i++) {
        if (i)
            *p++ = ':';
        p = put_hex(p, mac[i], 2, "0123456789ABCDEF");
    }
    return p;
}

static inline void copy(uint8_t *to, const uint8_t *from, int n)
{
    while (n--)
        *to++ = *from++;
}

static inline int same(const uint8_t *a, const uint8_t *b, int n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

static inline void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline unsigned get16(const uint8_t *p)
{
    return (unsigned)(p[0] << 8 | p[1]);
}

#endif
