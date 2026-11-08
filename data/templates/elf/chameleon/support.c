/*
 * What the Chameleon Cart firmware needs from the C library, so the game can be linked
 * with it without newlib (CC0, VCSIDE's ELF template). Only used by "make chameleon".
 */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

void srand(unsigned seed) { (void)seed; }
void __libc_init_array(void) {}

/* The firmware divides; libgcc's division in the game is renamed (see the Makefile). */
unsigned __aeabi_uidiv(unsigned a, unsigned b)
{
    unsigned q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = r << 1 | ((a >> i) & 1);
        if (r >= b) r -= b, q |= 1u << i;
    }
    return q;
}
