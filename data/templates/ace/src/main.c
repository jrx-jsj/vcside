/*
 * {{NAME}} - made with VCSIDE (ACE cartridge: C on the PlusCart's or UnoCart's ARM).
 *
 * A scrolling rainbow with a white bar you move with the joystick, and music: the song in
 * theme.vcsmusic (open it with the music editor; saving it writes music.h), with a
 * sound when the bar reaches an edge and another on the fire button. The ARM feeds the
 * 6507 every instruction of the visible lines (ace.h); during the overblank the 6507 runs
 * VSYNC and the blank lines from its own RAM and the ARM is free for the game logic.
 *
 * The music is played as samples: the ARM mixes the song's voices into one level per
 * scanline, written to AUDV0 (with AUDC0 = 0) on every line of the kernel and by the
 * overblank program on its lines. "fire" takes a voice while it sounds; "bump" plays on
 * the TIA's channel 1 instead, as a TIA sound: the kernel writes AUDC1, AUDF1 and AUDV1
 * from music_tia once per frame. RESET goes back to the cartridge's menu.
 *
 * Build: F7 (runs make). Run: F5 (Gopher2600). This template is CC0: use it as you like.
 */
#include "ace.h"
#include "music.h"

static int lines = {{VISIBLE_LINES}};    /* visible lines: the frame's lines - 63 of overblank */
static uint8_t swcha = 0xFF, swchb = 0xFF;  /* joysticks and console switches, read after the kernel
											(0xFF: nothing pressed until the first frame reads them) */
static uint8_t inpt4 = 0x80, last_inpt4 = 0x80;  /* fire button 0 (bit 7: 0 pressed; released until read) */
static int bar = 90, frame;

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

/* The visible lines. Between two ace_ calls the 6507 is already waiting for the next
 * instruction, so work is only done right after a WSYNC (or in the overblank). Each line
 * starts with its sample; the first 31 also give a byte of the overblank's samples. */
static void kernel(void)
{
	ace_overblank_end();
	for (int line = 0; line < lines - 1; line++) {
		ace_wsync();
		ace_write(AUDV0, music_level[line]);
		if (line < 31) ace_write((uint8_t)(0xE0 + line), music_nibbles[line]);
		if (line == 0) ace_write(VBLANK, 0);
		uint8_t colour = (line >= bar && line < bar + 8) ? 0x0E : (uint8_t)((((line >> 2) + frame) & 0x0F) << 4 | 0x06);
		ace_write(COLUBK, colour);
		ace_jmp();                /* once a line: the 6507's code stays at the same place */
	}
	ace_wsync();
	ace_write(AUDV0, music_level[lines - 1]);
	ace_write(VBLANK, 2);
	ace_write(AUDC1, music_tia[0]); /* "bump", on the TIA's channel 1 (silent when it ends) */
	ace_write(AUDF1, music_tia[1]);
	ace_write(AUDV1, music_tia[2]);
	ace_write(COLUBK, 0);
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
		int was = bar;
		if (!(swcha & 0x10) && bar > 0) bar--;            /* joystick 0 up */
		if (!(swcha & 0x20) && bar < lines - 9) bar++;    /* joystick 0 down */
		if (bar != was && (bar == 0 || bar == lines - 9)) music_effect(MUSIC_EFFECT_BUMP);
		if (!(inpt4 & 0x80) && (last_inpt4 & 0x80)) music_effect(MUSIC_EFFECT_FIRE);
		last_inpt4 = inpt4;
		frame++;
		/* the frame's music: samples for the kernel's lines and for the overblank's */
		music_frame(lines + MUSIC_OVERBLANK_LINES);
		if (!(swchb & 0x01)) {               /* RESET: back to the menu */
			leave();
			return 1;
		}
		kernel();
	}
}
