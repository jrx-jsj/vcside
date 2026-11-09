/*
 * vcsLib.h - the vcsLib functions of an ELF cartridge (PlusCart, UnoCart, Chameleon Cart,
 * Stella, Gopher2600). Written for VCSIDE's ELF template: CC0 1.0, public domain.
 * The names and values follow the vcsLib interface as Stella (ElfEnvironment.cxx,
 * VcsLib.cxx), Al-Nafuur's TinyELF-Basic (vcsLib.h) and Gopher2600 define it; the text is
 * VCSIDE's own.
 *
 * The game is a relocatable ARM object. The loader links these functions; each one puts
 * 6507 instructions on the cartridge bus, so the 6507 runs what the ARM sends it. The
 * 6507 cycles of each call are in parentheses.
 */
#ifndef VCSLIB_H
#define VCSLIB_H

#include <stdint.h>

/* elf_main(uint32_t *args): what the cartridge says about the console */
#define MP_SYSTEM_TYPE    0
#define MP_CLOCK_HZ       1
#define MP_FEATURE_FLAGS  2

#define ST_NTSC_2600      0
#define ST_PAL_2600       1
#define ST_PAL60_2600     2
#define ST_NTSC_7800      3
#define ST_PAL_7800       4

#define FF_MULTI_CART     1   /* started from a menu: elf_main may return to it */
#define FF_CHAMELEON_CART 2   /* Chameleon Cart: elf_main starts during the overblank */
#define FF_REDUCED_HMOVE  4   /* late HMOVE moves one pixel less (test it with a mask) */

/* TIA (write) */
#define VSYNC   0x00
#define VBLANK  0x01
#define WSYNC   0x02
#define RSYNC   0x03
#define NUSIZ0  0x04
#define NUSIZ1  0x05
#define COLUP0  0x06
#define COLUP1  0x07
#define COLUPF  0x08
#define COLUBK  0x09
#define CTRLPF  0x0A
#define REFP0   0x0B
#define REFP1   0x0C
#define PF0     0x0D
#define PF1     0x0E
#define PF2     0x0F
#define RESP0   0x10
#define RESP1   0x11
#define RESM0   0x12
#define RESM1   0x13
#define RESBL   0x14
#define AUDC0   0x15
#define AUDC1   0x16
#define AUDF0   0x17
#define AUDF1   0x18
#define AUDV0   0x19
#define AUDV1   0x1A
#define GRP0    0x1B
#define GRP1    0x1C
#define ENAM0   0x1D
#define ENAM1   0x1E
#define ENABL   0x1F
#define HMP0    0x20
#define HMP1    0x21
#define HMM0    0x22
#define HMM1    0x23
#define HMBL    0x24
#define VDELP0  0x25
#define VDELP1  0x26
#define VDELBL  0x27
#define RESMP0  0x28
#define RESMP1  0x29
#define HMOVE   0x2A
#define HMCLR   0x2B
#define CXCLR   0x2C

/* TIA (read) and RIOT */
#define INPT4   0x0C
#define INPT5   0x0D
#define SWCHA   0x0280
#define SWCHB   0x0282
#define INTIM   0x0284
#define TIM64T  0x0296

/* The loader's functions are reached with long calls: on the PlusCart the game runs in
 * RAM and vcsLib stays in flash, too far for a BL. Only these calls are long. */
#pragma long_calls

void vcsJmp3(void);                              /* jmp $1000 (3): keeps the 6507 in its 4 KB */
void vcsNop2(void);                              /* nop (2) */
void vcsNop2n(uint16_t count);                   /* count nops (2 × count) */
void vcsLda2(uint8_t value);                     /* lda # (2) */
void vcsLdx2(uint8_t value);                     /* ldx # (2) */
void vcsLdy2(uint8_t value);                     /* ldy # (2) */
void vcsSta3(uint8_t zeroPage);                  /* sta zp (3) */
void vcsStx3(uint8_t zeroPage);                  /* stx zp (3) */
void vcsSty3(uint8_t zeroPage);                  /* sty zp (3) */
/* sta $00zp in its absolute form (4): a cycle more than vcsSta3. Stella also writes a whole
 * 16-bit address, but Gopher2600 and the Chameleon Cart take only the low byte. */
void vcsSta4(uint8_t zeroPage);
void vcsWrite5(uint8_t zeroPage, uint8_t value); /* lda # + sta zp (5) */
void vcsWrite6(uint16_t address, uint8_t value); /* lda # + sta abs (6) */
uint8_t vcsRead4(uint16_t address);              /* lda abs (4), returns what the 6507 read */

/* Overblank: the 6507 runs VSYNC, VBLANK and overscan on its own from the RIOT's RAM
 * (63 lines) and the ARM is free until vcsEndOverblank(). */
void vcsCopyOverblankToRiotRam(void);
void vcsStartOverblank(void);
void vcsEndOverblank(void);

/* Bus stuffing (vcsWrite3, vcsLd[axy]ForBusStuff2) exists on the PlusCart and in Stella
 * but not in the Chameleon Cart's current firmware, so it is left out here. */

void *memset(void *dst, int value, unsigned int count);
void *memcpy(void *dst, const void *src, unsigned int count);

#pragma long_calls_off

/* On the Chameleon Cart the game runs from flash, whose wait states can leave the 6507
 * without its next instruction. Code that feeds the 6507 outside the overblank goes to
 * RAM (.RamFunc, copied by the firmware at startup; the other loaders run everything
 * from RAM anyway). RAM_FUNC for functions called from flash, RAM_K for the ones only
 * called from other RAM functions. */
#define RAM_FUNC __attribute__((noinline, long_call, section(".RamFunc")))
#define RAM_K    __attribute__((section(".RamFunc")))

#endif
