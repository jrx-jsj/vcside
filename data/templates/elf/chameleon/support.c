/*
 * What the Chameleon Cart firmware needs from the C library, so the game can be linked
 * with it without newlib (CC0, VCSIDE's ELF template). Only used by "make chameleon".
 */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t count)
{
	uint8_t *to = dst;
	const uint8_t *from = src;
	while (count--) *to++ = *from++;
	return dst;
}

void *memset(void *dst, int value, size_t count)
{
	uint8_t *to = dst;
	while (count--) *to++ = (uint8_t)value;
	return dst;
}

void srand(unsigned seed) { (void)seed; }
void __libc_init_array(void) {}

/* The firmware divides; libgcc's division in the game is renamed (see the Makefile). */
unsigned __aeabi_uidiv(unsigned dividend, unsigned divisor)
{
	unsigned quotient = 0, remainder = 0;
	for (int bit = 31; bit >= 0; bit--) {
		remainder = remainder << 1 | ((dividend >> bit) & 1);
		if (remainder >= divisor) remainder -= divisor, quotient |= 1u << bit;
	}
	return quotient;
}
