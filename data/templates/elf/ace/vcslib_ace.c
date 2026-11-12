/*
 * {{NAME}} - vcsLib on an ACE cartridge (make ace): the same functions the ELF loaders
 * give the game, written on the ACE bus library (ace.c), so src/ builds unchanged.
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * Every function feeds the 6507 the same instructions as on an ELF cartridge, with the
 * same cycles, except vcsRead4: here it is "lda" and then "sta $1FF0", which the ARM reads
 * back (7 cycles, or 8 above $FF, instead of 4). Read inputs where a few more cycles do
 * not matter, as the templates do after the visible lines.
 */
#include "vcsLib.h"

/* from ace.h, which is not included here: its register names would clash with vcsLib.h's */
void ace_lda(uint8_t value);
void ace_sta(uint8_t address);
void ace_write(uint8_t address, uint8_t value);
void ace_nops(int count);
void ace_code(const uint8_t* bytes, int count);
void ace_jmp(void);
uint8_t ace_read(uint16_t address);
void ace_overblank_load(void);
void ace_overblank_start(void);
void ace_overblank_end(void);

void vcsJmp3(void) { ace_jmp(); }
void vcsNop2(void) { ace_nops(1); }
void vcsNop2n(uint16_t count) { ace_nops(count); }
void vcsLda2(uint8_t value) { ace_lda(value); }
void vcsSta3(uint8_t zeroPage) { ace_sta(zeroPage); }
void vcsWrite5(uint8_t zeroPage, uint8_t value) { ace_write(zeroPage, value); }
uint8_t vcsRead4(uint16_t address) { return ace_read(address); }

void vcsLdx2(uint8_t value)
{
	const uint8_t code[] = {0xA2, value};
	ace_code(code, sizeof code);
}

void vcsLdy2(uint8_t value)
{
	const uint8_t code[] = {0xA0, value};
	ace_code(code, sizeof code);
}

void vcsStx3(uint8_t zeroPage)
{
	const uint8_t code[] = {0x86, zeroPage};
	ace_code(code, sizeof code);
}

void vcsSty3(uint8_t zeroPage)
{
	const uint8_t code[] = {0x84, zeroPage};
	ace_code(code, sizeof code);
}

void vcsSta4(uint8_t zeroPage)
{
	const uint8_t code[] = {0x8D, zeroPage, 0x00};   /* sta $00zp, absolute */
	ace_code(code, sizeof code);
}

void vcsWrite6(uint16_t address, uint8_t value)
{
	const uint8_t code[] = {0xA9, value, 0x8D, (uint8_t)address, (uint8_t)(address >> 8)};
	ace_code(code, sizeof code);
}

/* The ACE overblank program has the ELF loaders' layout (samples at $E0-$FE, 63 lines) but
 * writes AUDV0 itself: the games' check for "sta $2d" at $86-$87 finds something else and
 * leaves it alone. */
void vcsCopyOverblankToRiotRam(void) { ace_overblank_load(); }
void vcsStartOverblank(void) { ace_overblank_start(); }
void vcsEndOverblank(void) { ace_overblank_end(); }
