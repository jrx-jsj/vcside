/*
 * {{NAME}} - memcpy and memset, which the compiler calls by itself (struct copies,
 * initialisers). The program has no C library: the Arm toolchain may come without one.
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * The Makefile builds with -fno-tree-loop-distribute-patterns, so these loops are not
 * turned back into calls to themselves.
 */
#include <stddef.h>
#include <stdint.h>

void* memcpy(void* destination, const void* source, size_t size)
{
    uint8_t* to = destination;
    const uint8_t* from = source;
    while (size--) *to++ = *from++;
    return destination;
}

void* memset(void* destination, int value, size_t size)
{
    uint8_t* to = destination;
    while (size--) *to++ = (uint8_t)value;
    return destination;
}
