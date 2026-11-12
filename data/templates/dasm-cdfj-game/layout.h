/* {{NAME}} - what the 6507 and the ARM share in the display data
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * Offsets in the display data (4K of the cartridge's RAM), and the data streams the
 * kernels read. game.c includes this file and the Makefile turns it into
 * build/layout.asm for main.asm: keep one "#define NAME number" (decimal) per line.
 */

/* the 6507 writes them before calling the ARM (CallArm) */
#define LAYOUT_FUNCTION 0       /* what the ARM is asked to do: FUNCTION_START or FUNCTION_FRAME */
#define LAYOUT_SWCHA 1          /* the joysticks */
#define LAYOUT_INPT4 2          /* the left fire button (bit 7 = 0: down) */
#define LAYOUT_SWCHB 3          /* the console switches (bit 0 = 0: RESET down) */
#define LAYOUT_SYSTEM 4         /* the TV system: 0 NTSC, 1 PAL60, 2 PAL */
#define LAYOUT_CXP0FB 5         /* the collisions on the frame just drawn: bit 7 hero-playfield */
#define LAYOUT_CXP1FB 6         /* bit 7 enemy-playfield, bit 6 enemy-shot (the ball) */
#define LAYOUT_CXBLPF 7         /* bit 7 shot-playfield */
#define LAYOUT_CXPPMM 8         /* bit 7 hero-enemy */

/* the ARM writes them for the 6507, which reads them in order with DSCOMM: the frame's
 * registers (game.c, send_registers), then a few more in the kernels */
#define LAYOUT_OUT 9

/* the ARM's alone */
#define LAYOUT_WAVES 32         /* the three voices' waveforms, 32 samples each */
#define LAYOUT_BUFFERS 256      /* the streams' data, a byte per line or pass (game.c) */

/* data streams: "lda #STREAM_x" in a kernel reads the next byte of one */
#define STREAM_CART 0           /* the title: the cartridge's PF0, PF1, PF2, PF0, PF1, PF2 (0-5) */
#define STREAM_CART_COLOUR 6    /* its rows' COLUPF */
#define STREAM_CART_SIDES 7     /* ENAM0 and ENAM1: the label's sides */
#define STREAM_THE_GAME 8       /* THE GAME's six columns (8-13) */
#define STREAM_PLAY 14          /* PLAY's PF0, PF1, PF2, PF0, PF1, PF2 (14-19) */
#define STREAM_SYSTEM 20        /* the TV system's text (20-25) */
#define STREAM_ROOM 0           /* the game: the room's PF0, PF1, PF2 (0-2), a byte per pass */
#define STREAM_HERO 3           /* GRP0 */
#define STREAM_HERO_COLOUR 4    /* COLUP0 */
#define STREAM_ENEMY 5          /* GRP1 */
#define STREAM_SHOT 6           /* ENABL */
#define STREAM_SCORE 7          /* the score's PF1, a byte per line */
#define STREAM_LIVES 8          /* the lives' PF1 */

#define FUNCTION_START 0        /* power on: the song from its start, the title screen */
#define FUNCTION_FRAME 1        /* a frame: the game's logic, the music, what to draw */
