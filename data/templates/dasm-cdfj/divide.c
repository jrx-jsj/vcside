/* {{NAME}} - division for the C code
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * The ARM7 has no divide instruction: the compiler calls these functions for "/" and "%".
 * libgcc has them too, but as ARM code, and Stella's and Gopher2600's ARM emulators run
 * only Thumb code for CDFJ, so the game has its own (the linker takes them first).
 * The quotient comes back in r0 and the remainder in r1: the low and high halves of a
 * 64-bit result.
 */
#include <stdint.h>

uint64_t __aeabi_uidivmod(uint32_t numerator, uint32_t denominator);
uint32_t __aeabi_uidiv(uint32_t numerator, uint32_t denominator);
uint64_t __aeabi_idivmod(int32_t numerator, int32_t denominator);
int32_t __aeabi_idiv(int32_t numerator, int32_t denominator);

uint64_t __aeabi_uidivmod(uint32_t numerator, uint32_t denominator)
{
    uint32_t quotient = 0, remainder = 0;
    if (denominator == 0) return 0xFFFFFFFFu;   /* nothing sensible: all ones, no remainder */
    for (int bit = 31; bit >= 0; bit--) {      /* long division, a bit at a time */
        remainder = remainder << 1 | (numerator >> bit & 1);
        if (remainder >= denominator) {
            remainder -= denominator;
            quotient |= 1u << bit;
        }
    }
    return (uint64_t)remainder << 32 | quotient;
}

uint32_t __aeabi_uidiv(uint32_t numerator, uint32_t denominator)
{
    return (uint32_t)__aeabi_uidivmod(numerator, denominator);
}

/* Signed: the quotient rounds towards 0, the remainder has the numerator's sign. */
uint64_t __aeabi_idivmod(int32_t numerator, int32_t denominator)
{
    uint32_t positive_numerator = numerator < 0 ? 0u - (uint32_t)numerator : (uint32_t)numerator;
    uint32_t positive_denominator = denominator < 0 ? 0u - (uint32_t)denominator : (uint32_t)denominator;
    uint64_t result = __aeabi_uidivmod(positive_numerator, positive_denominator);
    uint32_t quotient = (uint32_t)result, remainder = (uint32_t)(result >> 32);
    if ((numerator < 0) != (denominator < 0)) quotient = 0u - quotient;
    if (numerator < 0) remainder = 0u - remainder;
    return (uint64_t)remainder << 32 | quotient;
}

int32_t __aeabi_idiv(int32_t numerator, int32_t denominator)
{
    return (int32_t)(uint32_t)__aeabi_idivmod(numerator, denominator);
}
