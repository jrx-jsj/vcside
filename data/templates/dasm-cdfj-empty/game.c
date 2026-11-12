/* {{NAME}} - the game's C code, run by the cartridge's ARM
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * main.asm calls game_arm() through CALLFN (start.S is the way in): once at power on, then
 * in the vertical blank of every frame. They talk through the display data (layout.h):
 * the 6507 leaves the joystick and the button there; the C code can leave data for the
 * data streams, and values the 6507 reads in order with DSCOMM.
 *
 * Sound: the cartridge plays three voices, each a 32-sample waveform in the display data
 * at a note set with the driver (cdfj.h); the 6507 copies their sum to AUDV0 on every line.
 * start() leaves them silent.
 */
#include <stdint.h>

#include "cdfj.h"
#include "layout.h"

static void start(void)
{
    for (uint8_t *byte = __bss_start; byte < __bss_end; byte++) *byte = 0;
    uint8_t *display = CDFJ_DISPLAY;
    for (int voice = 0; voice < 3; voice++) {
        for (int i = 0; i < 32; i++) display[LAYOUT_WAVES + 32 * voice + i] = 0;
        cdfj_set_wave_size(voice, 27);  /* 32 samples */
        cdfj_waveform(voice, LAYOUT_WAVES + 32 * voice);
        cdfj_set_note(voice, 0);
    }
}

static void frame(uint8_t swcha, uint8_t inpt4)
{
    /* the game logic goes here */
    (void)swcha;
    (void)inpt4;
}

void game_arm(void)
{
    uint8_t *display = CDFJ_DISPLAY;
    if (display[LAYOUT_FUNCTION] == FUNCTION_START) start();
    else frame(display[LAYOUT_SWCHA], display[LAYOUT_INPT4]);
    cdfj_step(CDFJ_COMM_STREAM, 1, 0);  /* the 6507 reads what follows with DSCOMM */
}
