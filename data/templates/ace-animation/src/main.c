/*
 * {{NAME}} - made with VCSIDE (ACE cartridge: C on the PlusCart's or UnoCart's ARM).
 *
 * An animated sprite that walks with the joystick. Its frames, colours and animations are
 * drawn in hero.vcsgfx (open it in VCSIDE): every save writes hero.gfx.h, which this file
 * includes. The "idle" animation blinks now and then; "walk" plays while it moves.
 *
 * The music is the song in theme.vcsmusic (open it with the music editor; saving it writes
 * theme.music.h): a sound plays when the hero reaches an edge and another on the fire
 * button. It is played as samples: the ARM mixes the song's voices into one level per
 * scanline, written to AUDV0 (with AUDC0 = 0) on every line of the kernel and by the
 * overblank program on its lines. "fire" takes a voice while it sounds; "bump" plays on
 * the TIA's channel 1 instead, as a TIA sound: the kernel writes AUDC1, AUDF1 and AUDV1
 * from music_tia once per frame.
 *
 * The ARM feeds the 6507 every instruction of the visible lines (ace.h); during the
 * overblank the 6507 runs VSYNC and the blank lines from its own RAM and the ARM runs the
 * game logic. RESET goes back to the cartridge's menu.
 *
 * Build: F7 (runs make). Run: F5 (Gopher2600). This template is CC0: use it as you like.
 */
#include "ace.h"
#include "hero.gfx.h"
#include "theme.music.h"

#define LINES_PER_ROW 2          /* each row of the sprite is drawn on two lines */

static int lines = {{VISIBLE_LINES}};    /* visible lines: the frame's lines - 63 of overblank */
static uint8_t swcha, swchb;     /* joysticks and console switches, read after the kernel */
static uint8_t inpt4, last_inpt4 = 0x80;  /* fire button 0 (bit 7: 0 pressed) */
static int hero_x = 76, hero_y = 90, facing_left, frame;
static int hero_frame;           /* the frame the kernel draws */
static vcsgfx_player hero;       /* the animation that is playing and where it is */

/* Starts the 6507: at $1000, TIA and RAM cleared (AUDC0 = 0 too: AUDV0 is then a plain
 * level), the overblank program in its RAM, and the first overblank. */
static void start_6507(void)
{
    ace_start();
    ace_lda(0);
    for (int address = 0; address < 0x40; address++)
        if (address != WSYNC && address != RSYNC) ace_sta((uint8_t)address);
    for (int address = 0x80; address < 0x100; address++)
        ace_sta((uint8_t)address);
    ace_overblank_load();
    ace_overblank_start();
}

/* Puts player 0 at x (0-159): RESP0 on the right cycle of a line, then HMOVE for the
 * last pixels. RESP0 after n nops puts it at 6n - 54 (from 10 nops on: earlier, it would
 * still be in the horizontal blank), so it goes 1-6 pixels to the right of x and HMOVE
 * takes it back. HMP0 is set first: after RESP0 there is no room for it when x is near the
 * right edge. One line, and the next starts with the HMOVE. */
static void position_p0(int x)
{
    ace_write(HMP0, (uint8_t)((6 - x % 6) << 4));  /* positive: to the left */
    ace_wsync();
    ace_nops(10 + x / 6);
    ace_sta(RESP0);
    ace_wsync();
    ace_sta(HMOVE);
}

/* The visible lines. Between two ace_ calls the 6507 is already waiting for the next
 * instruction, so work is only done right after a WSYNC (or in the overblank). Each line
 * starts with its sample (line 0's goes just before it: that line positions the hero);
 * lines 2 to 32 also take a byte of the overblank's samples. */
static void kernel(void)
{
    const uint8_t *gfx = hero_P0_gfx[hero_frame], *colour = hero_P0_colour[hero_frame];
    ace_overblank_end();
    ace_write(NUSIZ0, HERO_P0_NUSIZ);
    ace_write(REFP0, facing_left ? 0x08 : 0x00);
    ace_write(AUDV0, music_level[0]);
    position_p0(hero_x);
    ace_write(AUDV0, music_level[1]);
    ace_jmp();
    for (int line = 2; line < lines - 1; line++) {
        ace_wsync();
        ace_write(AUDV0, music_level[line]);
        if (line < 2 + 31) ace_write((uint8_t)(0xE0 + line - 2), music_nibbles[line - 2]);
        if (line == 2) ace_write(VBLANK, 0);
        unsigned row = (unsigned)(line - hero_y) / LINES_PER_ROW;
        if (line >= hero_y && row < HERO_P0_HEIGHT) {
            ace_write(GRP0, gfx[row]);
            ace_write(COLUP0, colour[row]);
        } else {
            ace_write(GRP0, 0);
        }
        ace_jmp();                /* once a line: the 6507's code stays at the same place */
    }
    ace_wsync();
    ace_write(AUDV0, music_level[lines - 1]);
    ace_write(VBLANK, 2);
    ace_write(AUDC1, music_tia[0]); /* "bump", on the TIA's channel 1 (silent when it ends) */
    ace_write(AUDF1, music_tia[1]);
    ace_write(AUDV1, music_tia[2]);
    ace_write(GRP0, 0);
    swcha = ace_read(SWCHA);
    swchb = ace_read(SWCHB);
    inpt4 = ace_read(INPT4);
    ace_overblank_start();
}

/* Back to the menu: silence, then the 6507 is left reading the reset vector. */
static void leave(void)
{
    ace_overblank_end();
    ace_write(VBLANK, 2);
    ace_write(AUDV0, 0);
    ace_write(AUDV1, 0);
    ace_leave();
}

int main(void)
{
    music_init();
    start_6507();
    for (;;) {
        /* Overblank: the game logic goes here. */
        int moving = 0, bottom = lines - 1 - HERO_P0_HEIGHT * LINES_PER_ROW;
        int was_at_edge = hero_x == 0 || hero_x == 160 - 8 || hero_y == 2 || hero_y == bottom;
        if (!(swcha & 0x80) && hero_x < 160 - 8) hero_x++, facing_left = 0, moving = 1;   /* right */
        if (!(swcha & 0x40) && hero_x > 0) hero_x--, facing_left = 1, moving = 1;         /* left */
        if (!(swcha & 0x10) && hero_y > 2) hero_y--, moving = 1;                          /* up */
        if (!(swcha & 0x20) && hero_y < bottom) hero_y++, moving = 1;                     /* down */
        if (!was_at_edge && (hero_x == 0 || hero_x == 160 - 8 || hero_y == 2 || hero_y == bottom))
            music_effect(MUSIC_EFFECT_BUMP);              /* it has just reached an edge */
        if (!(inpt4 & 0x80) && (last_inpt4 & 0x80)) music_effect(MUSIC_EFFECT_FIRE);
        last_inpt4 = inpt4;
        vcsgfx_play(&hero, moving ? &hero_walk : &hero_idle);
        hero_frame = vcsgfx_animate(&hero);
        frame++;
        /* the frame's music: samples for the kernel's lines and for the overblank's */
        music_frame(lines + MUSIC_OVERBLANK_LINES);
        if (frame > 1 && !(swchb & 0x01)) {               /* RESET: back to the menu */
            leave();
            return 1;
        }
        kernel();
    }
}
