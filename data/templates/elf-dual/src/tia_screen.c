/*
 * {{NAME}} - the Atari 2600's screen: the TIA draws the room with the playfield and the hero
 * with player 0. The ARM feeds the 6507 every instruction of the visible lines through vcsLib;
 * during the overblank the 6507 runs VSYNC, VBLANK and overscan by itself and the ARM runs the
 * game. CC0: use it as you like.
 *
 * Rules of the kernel: between two vcsLib calls the 6507 is already waiting, so work is done
 * right after a vcsSta3(WSYNC) or in the overblank; everything the 6507 waits on is in RAM
 * (RAM_FUNC, RAM_K: code in the Chameleon's flash is too slow); vcsJmp3() once a line keeps
 * its program counter in its 4 KB.
 */
#include "vcsLib.h"
#include "game.h"
#include "screens.h"

static int lines = 199;          /* visible lines: 262 - 63 of overblank (NTSC, PAL60), 249 on PAL */
static int room_top;             /* the room's first line, centred */
static int multicart;            /* started from a menu (PlusCart, UnoCart): RESET goes back */
static uint8_t swcha = 0xFF, swchb = 0xFF, inpt4 = 0x80;  /* nothing pressed until the first frame */

/* Back to $1000, the TIA and the RIOT's RAM cleared, then the overblank program. */
static RAM_FUNC void start_6507(void)
{
	vcsJmp3();
	vcsLda2(0);
	for (int i = 0; i < 256; i++)
		vcsSta3((uint8_t)i);
	vcsCopyOverblankToRiotRam();
	vcsStartOverblank();          /* nothing long before this: the 6507 would run out of code */
}

/* Player 0 at x (0-159): RESP0 after 10 + x/6 nops lands 1-6 pixels to the right of x and
 * HMOVE takes it back. Two lines; the second starts with the HMOVE. */
static RAM_K void position_p0(int x)
{
	vcsWrite5(HMP0, (uint8_t)((6 - x % 6) << 4));
	vcsSta3(WSYNC);
	vcsNop2n((uint16_t)(10 + x / 6));
	vcsSta3(RESP0);
	vcsSta3(WSYNC);
	vcsSta3(HMOVE);
}

/* The playfield of a room line: full walls in the top and bottom bands, the side walls
 * (PF0 bit 4, mirrored by CTRLPF) between them. */
static RAM_K void room_walls(int y)
{
	int band = y < WALL_LINES || y >= ROOM_LINES - WALL_LINES;
	vcsWrite5(PF0, band ? 0xF0 : 0x10);
	vcsWrite5(PF1, band ? 0xFF : 0x00);
	vcsWrite5(PF2, band ? 0xFF : 0x00);
}

static RAM_FUNC void kernel(void)
{
	const uint8_t *gfx = game.hero_gfx, *colour = game.hero_colour;
	int hero_top = room_top + game.hero_y, hero_end = hero_top + game.hero_rows * LINES_PER_ROW;
	vcsEndOverblank();
	vcsWrite5(REFP0, game.facing_left ? 0x08 : 0x00);
	vcsWrite5(CTRLPF, 0x01);
	vcsWrite5(COLUPF, WALL_COLOUR);
	vcsWrite5(COLUBK, 0x00);
	position_p0(game.hero_x);    /* lines 0 and 1 */
	vcsJmp3();
	for (int i = 2; i < lines - 1; i++) {
		vcsSta3(WSYNC);
		if (i == 2) vcsWrite5(VBLANK, 0);
		int y = i - room_top;
		/* At most five writes per line, each before the beam reaches what it changes (the
		 * line's first pixel is at about cycle 23): the background, then the hero (from
		 * pixel 4 on, cycle 24), then PF0, PF1 (pixel 16, cycle 28) and PF2. The hero can
		 * start or end on the lines where the walls change. */
		if (y == 0 || y == ROOM_LINES) vcsWrite5(COLUBK, y == 0 ? FLOOR_COLOUR : 0x00);
		if (i >= hero_top && i < hero_end) {
			int row = (i - hero_top) / LINES_PER_ROW;
			vcsWrite5(GRP0, gfx[row]);
			vcsWrite5(COLUP0, colour[row]);
		} else if (i == hero_end) {
			vcsWrite5(GRP0, 0);
		}
		if (y == 0 || y == WALL_LINES || y == ROOM_LINES - WALL_LINES) room_walls(y);
		if (y == ROOM_LINES) {
			vcsWrite5(PF0, 0);
			vcsWrite5(PF1, 0);
			vcsWrite5(PF2, 0);
		}
		vcsJmp3();
	}
	vcsSta3(WSYNC);
	vcsWrite5(VBLANK, 2);
	vcsWrite5(GRP0, 0);
	/* the music of the frame, on both TIA channels */
	vcsWrite5(AUDC0, game.audio[0]);
	vcsWrite5(AUDF0, game.audio[1]);
	vcsWrite5(AUDV0, game.audio[2]);
	vcsWrite5(AUDC1, game.audio[3]);
	vcsWrite5(AUDF1, game.audio[4]);
	vcsWrite5(AUDV1, game.audio[5]);
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

int tia_screen_run(uint32_t *args)
{
	if (args[MP_SYSTEM_TYPE] == ST_PAL_2600) lines = 312 - 63;
	room_top = (lines - ROOM_LINES) / 2;
	multicart = (args[MP_FEATURE_FLAGS] & FF_MULTI_CART) != 0;
	/* The Chameleon Cart calls elf_main with the 6507 already in an overblank. */
	if (args[MP_FEATURE_FLAGS] & FF_CHAMELEON_CART) vcsEndOverblank();
	start_6507();
	for (;;) {
		/* Overblank: the game */
		GameInput input = {0};
		input.right = !(swcha & 0x80);
		input.left = !(swcha & 0x40);
		input.down = !(swcha & 0x20);
		input.up = !(swcha & 0x10);
		input.fire = !(inpt4 & 0x80);
		game_frame(&input);
		if (multicart && !(swchb & 0x01)) {   /* RESET: back to the menu */
			leave();
			return 0;
		}
		kernel();
	}
}
