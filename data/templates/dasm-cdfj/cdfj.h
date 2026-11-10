/* {{NAME}} - the CDFJ cartridge as the ARM sees it
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * Harmony/Melody: flash from address 0 (the ROM image: the driver, then this C code from
 * $0800), RAM from $40000000: the driver's 2K, the display data (4K) and 2K for the C
 * code's variables and stack.
 */
#pragma once
#include <stdint.h>

/* The display data: the 6507 reads it through the data streams, and writes it with DSWRITE. */
#define CDFJ_DISPLAY ((uint8_t *)0x40000800)

/* Per data stream (0-31; 32 is DSCOMM, the one DSPTR and DSWRITE move): where it reads,
 * as its offset in the display data << 20, and what each read adds, whole << 8 | 256ths. */
#define CDFJ_STREAM_POINTER ((volatile uint32_t *)0x40000098)
#define CDFJ_STREAM_STEP ((volatile uint32_t *)0x40000124)
#define CDFJ_COMM_STREAM 32

/* Per voice (0-2): the address of its waveform. */
#define CDFJ_WAVEFORM ((volatile uint32_t *)0x400001B0)

static inline void cdfj_point(int stream, unsigned offset) { CDFJ_STREAM_POINTER[stream] = (uint32_t)offset << 20; }
static inline void cdfj_step(int stream, unsigned whole, unsigned fraction) { CDFJ_STREAM_STEP[stream] = whole << 8 | fraction; }
static inline void cdfj_waveform(int voice, unsigned offset) { CDFJ_WAVEFORM[voice] = 0x40000800u + offset; }

/* The driver's functions, called by start.S. */
void cdfj_set_note(int voice, uint32_t frequency);  /* frequency: 2^32 x Hz / 20000 */
void cdfj_reset_wave(int voice);                    /* the waveform from its first sample */
void cdfj_set_wave_size(int voice, int shift);      /* 27: 32 samples (the default) */

/* The variables, which start() clears (arm.ld). */
extern uint8_t __bss_start[], __bss_end[];
