/*
 * {{NAME}} - the ACE bus library: the cartridge's ARM feeds the 6507 its instructions.
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * An ACE program is the cartridge: every time the 6507 reads an address of the
 * cartridge ($1000-$1FFF), the ARM must put a byte on the data bus. This library keeps
 * track of the address the 6507 will fetch next and gives it, byte by byte, the
 * instructions the game asks for (ace_write, ace_read...). Between two calls the 6507
 * waits on the bus, so the ARM only has time for work right after an ace_wsync() (until
 * the end of that line) or while the 6507 runs the overblank program from its own RAM.
 *
 * Runs on the PlusCart and the UnoCart (ACE-UF00), and in Gopher2600.
 */
#ifndef ACE_H
#define ACE_H

#include <stdbool.h>
#include <stdint.h>

/* The words the firmware leaves at 0x20000000 before calling the program. */
typedef struct {
	const uint8_t* file;                 /* where the ACE file is (in flash) */
	uint32_t* ccm;                       /* the CCM RAM the firmware leaves free */
	int (*reboot_into_cartridge)(void);  /* resets the console into this cartridge */
	int (*back_to_menu)(void);           /* the firmware's own cartridge: its menu */
	uint32_t clock_hz;                   /* the ARM's clock */
	uint32_t ace_version;                /* 0x00MMmmbb */
	uint32_t firmware_version;           /* 0x00MMmmbb */
	volatile uint16_t* address_in;       /* the 6507's address bus (13 bits) */
	volatile uint8_t* data_in;           /* the data bus, as an input */
	volatile uint8_t* data_out;          /* the data bus, as an output */
	volatile uint16_t* data_mode;        /* 0x5555: data pins drive the bus; 0: let go */
	uint32_t end_mark;                   /* 0xACE42600 */
} AceArguments;

#define ACE_ARGUMENTS ((const AceArguments*)0x20000000)

/* TIA write registers */
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
/* TIA read registers */
#define CXM0P   0x00
#define CXM1P   0x01
#define CXP0FB  0x02
#define CXP1FB  0x03
#define CXM0FB  0x04
#define CXM1FB  0x05
#define CXBLPF  0x06
#define CXPPMM  0x07
#define INPT0   0x08
#define INPT1   0x09
#define INPT2   0x0A
#define INPT3   0x0B
#define INPT4   0x0C
#define INPT5   0x0D
/* RIOT */
#define SWCHA   0x280
#define SWACNT  0x281
#define SWCHB   0x282
#define SWBCNT  0x283
#define INTIM   0x284
#define TIMINT  0x285
#define TIM1T   0x294
#define TIM8T   0x295
#define TIM64T  0x296
#define T1024T  0x297

/* Lines the overblank program takes: VSYNC and the blank lines around it. */
#define ACE_OVERBLANK_LINES 63

/* Takes the 6507 over (start.S has already asked the firmware to reboot the console into
 * this cartridge): from here on it fetches its code at $1000. */
void ace_start(void);

/* Instructions for the 6507 (cycles in brackets). Each returns once the 6507 has
 * fetched the last byte, so the next call follows it without a gap. */
void ace_lda(uint8_t value);                  /* LDA #value (2) */
void ace_sta(uint8_t address);                /* STA address: A to a TIA register or RAM (3) */
void ace_write(uint8_t address, uint8_t value); /* LDA #value, STA address (5) */
void ace_wsync(void);                         /* STA WSYNC (3): the 6507 waits for the line's end */
void ace_nops(int count);                     /* NOP, count times (2 each) */
/* any instruction: its bytes, in order (no jumps or branches: the library must know
 * where the 6507 fetches next) */
void ace_code(const uint8_t* bytes, int count);
/* JMP $1000 (3): call once a line, or the library does it when the 6507 nears the end
 * of its 4 KB */
void ace_jmp(void);
/* LDA address, STA $1FF0 (7, or 8 above $FF): a TIA, RIOT or RAM value for the ARM */
uint8_t ace_read(uint16_t address);

/* Back to the PlusCart's or UnoCart's menu: the 6507 is left reading the reset vector;
 * then return 1 from main() at once, and the start code hands the bus to the firmware.
 * (In Gopher2600 the firmware is not there: the program starts again.) */
void ace_leave(void);

/* The overblank program: 63 lines (VSYNC on the first three) that the 6507 runs from its
 * own RAM ($80-$AB) while the ARM is free, playing a 4-bit sample per line from the bytes
 * at $E0-$FE (the low nibble, then the high one; the first line repeats the first). */
void ace_overblank_load(void);                /* once, before the first frame */
void ace_overblank_start(void);               /* JMP $0080: the ARM is free from here ... */
void ace_overblank_end(void);                 /* ... until the 6507 is back at $1000 */

#endif
