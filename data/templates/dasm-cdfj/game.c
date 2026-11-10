/* {{NAME}} - the game's C code, run by the cartridge's ARM
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * main.asm calls game_arm() through CALLFN (start.S is the way in): once at power on, then
 * at the start of every frame. They talk through the display data (layout.h): the 6507
 * leaves the joystick and the button there, and takes the square's x and its graphics
 * (data stream 0, one byte per line).
 *
 * The music: theme.vcsmusic (open it from the project panel) is the song, played as
 * samples with three voices, and its two sound effects; saving it writes theme.music.h.
 * The cartridge plays the voices: each one's waveform (32 samples) in the display data,
 * its note set with the driver. The two macros below are how the player gives them.
 * "fire" takes a voice while it sounds; "bump" plays on the TIA's channel 1 instead, as a
 * TIA sound: the player leaves AUDC1, AUDF1 and AUDV1 in music_tia, for main.asm to write.
 */
#include <stdint.h>

#include "cdfj.h"
#include "layout.h"

static void voice_waveform(int voice, const uint8_t *samples);
static void voice_note(int voice, uint32_t frequency, int restart);
#define MUSIC_CDFJ_WAVE(voice, samples) voice_waveform(voice, samples)
#define MUSIC_CDFJ_NOTE(voice, frequency, restart) voice_note(voice, frequency, restart)
#include "theme.music.h"

#define HERO_STREAM 0           /* data stream 0: the square, read by the kernel */
#define HERO_HEIGHT 12          /* lines */
#define HERO_RIGHT 149          /* the last x: PosObject (main.asm) draws 3 pixels further right */

static uint8_t hero_x, hero_y, last_button;

/* A voice's samples, to its 32 bytes of the display data, where the cartridge reads them. */
static void voice_waveform(int voice, const uint8_t *samples)
{
    uint8_t *wave = CDFJ_DISPLAY + LAYOUT_WAVES + 32 * voice;
    for (int i = 0; i < 32; i++) wave[i] = samples[i];
    cdfj_waveform(voice, LAYOUT_WAVES + 32 * voice);
}

static void voice_note(int voice, uint32_t frequency, int restart)
{
    cdfj_set_note(voice, frequency);
    if (restart) cdfj_reset_wave(voice);
}

static void start(int lines)
{
    for (uint8_t *byte = __bss_start; byte < __bss_end; byte++) *byte = 0;
    for (int voice = 0; voice < 3; voice++) cdfj_set_wave_size(voice, 27);
    music_init();
    hero_x = 76;
    hero_y = (uint8_t)(lines / 2);
    last_button = 0x80;         /* as if up: holding it at power on plays nothing */
}

static void frame(int lines, uint8_t swcha, uint8_t inpt4)
{
    /* the joystick moves the square; reaching the left or right edge plays "bump" */
    if (!(swcha & 0x80) && hero_x < HERO_RIGHT && ++hero_x == HERO_RIGHT) music_effect(MUSIC_EFFECT_BUMP);
    if (!(swcha & 0x40) && hero_x > 0 && --hero_x == 0) music_effect(MUSIC_EFFECT_BUMP);
    if (!(swcha & 0x20) && hero_y < lines - HERO_HEIGHT) hero_y++;
    if (!(swcha & 0x10) && hero_y > 0) hero_y--;
    /* the button going down plays "fire" */
    if (!(inpt4 & 0x80) && (last_button & 0x80)) music_effect(MUSIC_EFFECT_FIRE);
    last_button = inpt4;

    music_frame(lines);

    /* what the 6507 reads: the square's x, the TIA's channel 1, and the square's graphics
     * line by line */
    uint8_t *display = CDFJ_DISPLAY;
    for (int i = 0; i < 3; i++) display[LAYOUT_TIA + i] = music_tia[i];
    for (int line = 0; line < lines; line++)
        display[LAYOUT_HERO_GFX + line] = (unsigned)(line - hero_y) < HERO_HEIGHT ? 0xFF : 0x00;
    display[LAYOUT_HERO_X] = hero_x;
    cdfj_point(HERO_STREAM, LAYOUT_HERO_GFX);
    cdfj_step(HERO_STREAM, 1, 0);
}

void game_arm(void)
{
    uint8_t *display = CDFJ_DISPLAY;
    int lines = display[LAYOUT_LINES];
    if (display[LAYOUT_FUNCTION] == FUNCTION_START) start(lines);
    else frame(lines, display[LAYOUT_SWCHA], display[LAYOUT_INPT4]);
    cdfj_step(CDFJ_COMM_STREAM, 1, 0);  /* the 6507 reads what follows with DSCOMM */
}
