/* {{NAME}} - what the 6507 and the ARM share in the display data
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * Offsets in the display data (4K of the cartridge's RAM). game.c includes this file and
 * the Makefile turns it into build/layout.asm for main.asm: keep one "#define NAME number"
 * (decimal) per line.
 */

/* the 6507 writes them before calling the ARM (CallArm) */
#define LAYOUT_FUNCTION 0       /* what the ARM is asked to do: FUNCTION_START or FUNCTION_FRAME */
#define LAYOUT_SWCHA 1          /* the joysticks */
#define LAYOUT_INPT4 2          /* the left fire button (bit 7 = 0: down) */

/* the ARM's alone */
#define LAYOUT_WAVES 32         /* the three voices' waveforms, 32 samples each */

#define FUNCTION_START 0        /* power on */
#define FUNCTION_FRAME 1        /* a frame */
