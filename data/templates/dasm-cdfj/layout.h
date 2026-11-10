/* {{NAME}} - what the 6507 and the ARM share in the display data
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * Offsets in the display data (4K of the cartridge's RAM). game.c includes this file and
 * the Makefile turns it into build/layout.asm for main.asm: keep one "#define NAME number"
 * (decimal) per line.
 */

/* the 6507 writes them before calling the ARM */
#define LAYOUT_FUNCTION 0       /* what the ARM is asked to do: FUNCTION_START or FUNCTION_FRAME */
#define LAYOUT_SWCHA 1          /* the joysticks */
#define LAYOUT_INPT4 2          /* the left fire button (bit 7 = 0: down) */
#define LAYOUT_LINES 3          /* the kernel's lines (192 or 228) */

/* the ARM writes it for the 6507 */
#define LAYOUT_HERO_X 4         /* the square's x, in colour clocks */

/* the ARM's alone */
#define LAYOUT_WAVES 32         /* the three voices' waveforms, 32 samples each */
#define LAYOUT_HERO_GFX 256     /* the square's graphics, a byte per line (data stream 0) */

#define FUNCTION_START 0        /* power on: the song from its start, the square in the middle */
#define FUNCTION_FRAME 1        /* a frame: joystick, square, effects and music */
