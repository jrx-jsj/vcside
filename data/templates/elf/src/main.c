/*
 * {{NAME}} - made with VCSIDE (ELF cartridge, C on the cartridge's ARM).
 *
 * A scrolling rainbow with a white bar you move with the joystick, and music: the song in
 * theme.vcsmusic (open it with the music editor; saving it writes theme.music.h), with a
 * sound when the bar reaches an edge and another on the fire button. The ARM feeds the
 * 6507 every instruction of the visible lines; during the overblank the 6507 runs VSYNC,
 * VBLANK and overscan by itself and the ARM is free for the game logic.
 *
 * The music is played as samples: the ARM mixes the song's voices into one level per
 * scanline, written to AUDV0 (with AUDC0 = 0) on every line of the kernel and by the
 * overblank program on the overblank's lines. "fire" takes a voice while it sounds; "bump"
 * plays on the TIA's channel 1 instead, as a TIA sound: the kernel writes AUDC1, AUDF1 and
 * AUDV1 from music_tia once per frame.
 *
 * Build: F7 (runs make). Run: F5. This template is CC0: use it as you like.
 */
#include "vcsLib.h"
#include "theme.music.h"

static int lines = 199;          /* visible lines: 262 - 63 of overblank (NTSC, PAL60) */
static int multicart;            /* started from a menu (PlusCart, UnoCart): can go back */
/* joysticks and console switches, read after the kernel (0xFF: nothing pressed until the first frame reads them) */
static uint8_t swcha = 0xFF, swchb = 0xFF;
static uint8_t inpt4 = 0x80, last_inpt4 = 0x80;  /* fire button 0 (bit 7: 0 pressed; released until read) */
static int bar = 90, frame;

/* Starts the 6507: back to $1000, TIA and RIOT RAM cleared (AUDC0 = 0 too: AUDV0 is then
 * a plain level), then the overblank program. */
static RAM_FUNC void start_6507(void)
{
	vcsJmp3();
	vcsLda2(0);
	for (int i = 0; i < 256; i++)
		vcsSta3((uint8_t)i);
	vcsCopyOverblankToRiotRam();
	/* The overblank program writes a sample per line to $2D, which does nothing: its
	 * "sta $2d" (at $86-$87) becomes "sta AUDV0". Only if it is that program: another
	 * cartridge's may differ, and changing another byte would break it. */
	if (vcsRead4(0x86) == 0x85 && vcsRead4(0x87) == 0x2D) vcsWrite5(0x87, AUDV0);
	vcsStartOverblank();          /* nothing long before this: the 6507 would run out of code */
}

/* The visible lines. Between two vcsLib calls the 6507 is already waiting for the next
 * instruction, so work is only done right after a WSYNC (or in the overblank). Each line
 * starts with its sample; the first 31 also take a byte of the overblank's samples. */
static RAM_FUNC void kernel(void)
{
	vcsEndOverblank();
	for (int i = 0; i < lines - 1; i++) {
		vcsSta3(WSYNC);
		vcsWrite5(AUDV0, music_level[i]);
		if (i < 31) vcsWrite5((uint8_t)(0xE0 + i), music_nibbles[i]);
		if (i == 0) vcsWrite5(VBLANK, 0);
		uint8_t colour = (i >= bar && i < bar + 8) ? 0x0E : (uint8_t)((((i >> 2) + frame) & 0x0F) << 4 | 0x06);
		vcsWrite5(COLUBK, colour);
		vcsJmp3();                /* once a line: the 6507's PC must stay in its 4 KB */
	}
	vcsSta3(WSYNC);
	vcsWrite5(AUDV0, music_level[lines - 1]);
	vcsWrite5(VBLANK, 2);
	vcsWrite5(AUDC1, music_tia[0]); /* "bump", on the TIA's channel 1 (silent when it ends) */
	vcsWrite5(AUDF1, music_tia[1]);
	vcsWrite5(AUDV1, music_tia[2]);
	vcsWrite5(COLUBK, 0);
	swcha = vcsRead4(SWCHA);
	swchb = vcsRead4(SWCHB);
	inpt4 = vcsRead4(INPT4);
	vcsStartOverblank();
}

/* Back to the menu: the 6507 runs nops from $1000, as the menu left it. */
static RAM_FUNC void leave(void)
{
	vcsEndOverblank();
	vcsWrite5(VBLANK, 2);
	vcsWrite5(AUDV0, 0);
	vcsWrite5(AUDV1, 0);
	vcsJmp3();
	vcsNop2n(1024);
}

int elf_main(uint32_t *args)
{
	unsigned system = args[MP_SYSTEM_TYPE];
	if (system == ST_PAL_2600 || system == ST_PAL_7800) lines = 312 - 63;
	multicart = (args[MP_FEATURE_FLAGS] & FF_MULTI_CART) != 0;
	/* The Chameleon Cart calls elf_main with the 6507 already in an overblank. */
	if (args[MP_FEATURE_FLAGS] & FF_CHAMELEON_CART) vcsEndOverblank();
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
		if (multicart && !(swchb & 0x01)) {               /* RESET: back to the menu */
			leave();
			return 0;
		}
		kernel();
	}
}
