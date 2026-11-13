/*
 * {{NAME}} - VCSIDE The Game, made with VCSIDE (ACE cartridge: C on the PlusCart's or
 * UnoCart's ARM).
 *
 * A room with walls where the hero walks and shoots at two enemies: hitting one scores a
 * point, touching one costs a life, and after the last life the title screen comes back.
 * The title screen has a menu: PLAY, SYSTEM (NTSC, PAL60, PAL) and EXIT, back to the
 * cartridge's menu.
 *
 *   game.c    the rules (movement, walls, enemies, shot, score, lives)
 *   menu.c    the title screen's menu
 *   screen.c  what the kernels below draw, worked out in the overblank
 *   main.c    main, the kernels and the sound: everything that talks to the 6507 (ace.h)
 *
 * Graphics: hero.vcsgfx (player 0, idle and walk), enemy.vcsgfx (player 1), room.vcsgfx
 * (the room's playfield) and title.vcsgfx (the title's cartridge); saving one writes its
 * .gfx.h. Music: theme.vcsmusic (saving it writes music.h), with the effects shot,
 * hit and lose. It is played as samples: the ARM mixes the song's voices into one level per
 * scanline, written to AUDV0 (with AUDC0 = 0) on every line of the kernel and by the
 * overblank program on its own lines; "hit" plays on the TIA's channel 1 (music_tia).
 *
 * The ARM feeds the 6507 every instruction of the visible lines (ace.h); during the
 * overblank the 6507 runs VSYNC and the blank lines from its own RAM and the ARM runs the
 * game. The same game is in VCSIDE's ELF template, on vcsLib: only main.c differs.
 *
 * Build: F7 (runs make). Run: F5 (Gopher2600). This template is CC0: use it as you like.
 */
#include "ace.h"
#include "game.h"
#include "menu.h"
#include "screen.h"
#include "music.h"

#define DEFAULT_SYSTEM {{MENU_SYSTEM}}   /* the project's TV standard: ACE gives the program no system */

static int lines = 199;          /* visible lines: 262 - 63 of overblank (NTSC, PAL60), 312 - 63 (PAL) */
static int short_hmove;          /* the late HMOVE moves one pixel less (no menu row on ACE: 0) */
static uint8_t swcha = 0xFF, swchb = 0xFF;  /* joysticks and console switches, read after the kernel
											(0xFF: nothing pressed until the first frame reads them) */
static uint8_t inpt4 = 0x80;     /* fire button 0 (bit 7: 0 pressed; released until read) */
static const uint8_t *sample;    /* the next line's sample, in music_level */

static Menu menu;
static Game game;
static TitleScreen title;
static GameScreen screen;

/* ---- The 6507 --------------------------------------------------------------------------- */

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

/* Back to the menu: silence, then the 6507 is left reading the reset vector. */
static void leave(void)
{
	ace_overblank_end();
	ace_write(VBLANK, 2);
	ace_write(AUDV0, 0);
	ace_write(AUDV1, 0);
	ace_leave();
}

/* ldx #, ldy #, stx and sty: ace.h has no calls of their own for them. */
static void ldx(uint8_t value)
{
	const uint8_t code[] = {0xA2, value};
	ace_code(code, sizeof code);
}

static void ldy(uint8_t value)
{
	const uint8_t code[] = {0xA0, value};
	ace_code(code, sizeof code);
}

static void stx(uint8_t address)
{
	const uint8_t code[] = {0x86, address};
	ace_code(code, sizeof code);
}

static void sty(uint8_t address)
{
	const uint8_t code[] = {0x84, address};
	ace_code(code, sizeof code);
}

/* ---- Lines every kernel uses -----------------------------------------------------------
 * Between two ace_ calls the 6507 is already waiting for its next instruction, so these
 * only copy values worked out before; the cycle counts in the comments are where each
 * write ends, from the line's start. Every line writes its sample to AUDV0. */

/* A line with nothing new on it. */
static void blank_line(void)
{
	ace_sta(WSYNC);
	ace_write(AUDV0, *sample++);
	ace_jmp();                    /* once a line: the 6507's code stays at the same place */
}

/* Where an object goes: RESxx after "nops" nops puts a player at 6 x nops - 39 (the write
 * ends at cycle 8 + 2 x nops, and x = 3 x cycle - 63); a missile lands a pixel to the left
 * of a player, so missiles are placed at x + 1. Before cycle 24 it would still be in the
 * horizontal blank; after cycle 73 (x 152) the next WSYNC would fall on the next line. The
 * early HMOVE (at the start of a line) takes it back to x: HMxx > 0 moves it left. Worked
 * out in the overblank: between two ace_ calls there is no time for a division. */
typedef struct {
	int nops;
	uint8_t fine;               /* HMxx */
} Place;

static Place places[4];          /* the objects' places at the top of the frame */

static void place(Place *object, int x)
{
	int nops = (x + 39 + 5) / 6;  /* the first place at x or to its right */
	if (nops < 8) nops = 8;
	object->nops = nops;
	object->fine = (uint8_t)((6 * nops - 39 - x) << 4);
}

static void position_line(uint8_t reset, const Place *object)
{
	ace_sta(WSYNC);
	ace_write(AUDV0, *sample++);
	ace_nops(object->nops);
	ace_sta(reset);
}

/* The overblank program's 62 samples, as 31 bytes at $E0-$FE, on the first lines after it
 * (eight a line: AUDV0 and eight writes take 45 cycles). */
static void overblank_samples(int line)
{
	for (int i = 8 * line; i < 8 * line + 8 && i < 31; i++)
		ace_write((uint8_t)(0xE0 + i), music_nibbles[i]);
}

/* The last line: the beam off, the TIA's channel 1 for the "hit" effect, and the inputs. */
static void last_line(void)
{
	ace_sta(WSYNC);
	ace_write(AUDV0, *sample++);
	ace_write(VBLANK, 2);
	ace_write(AUDC1, music_tia[0]);
	ace_write(AUDF1, music_tia[1]);
	ace_write(AUDV1, music_tia[2]);
	swcha = ace_read(SWCHA);
	swchb = ace_read(SWCHB);
	inpt4 = ace_read(INPT4);
	ace_overblank_start();
}

/* ---- The title screen ------------------------------------------------------------------ */

#define TITLE_P0_X       56       /* 48 pixels of text: players 0 and 1 at 56 and 64, three */
#define TITLE_P1_X       64       /* copies 16 pixels apart each (NUSIZ = 3) */
#define CART_LEFT_SIDE   36       /* the label's sides: missiles 0 and 1, 4 pixels wide */
#define CART_RIGHT_SIDE  116

/* A line of 48 pixels: with VDELP0 and VDELP1 on, each player shows what its register had
 * when the other one was last written. The first three columns go early; the last three
 * just as the beam reaches them (cycles 45, 48, 51 and 54), from A, X and Y. The 48-pixel
 * technique of Activision's six-digit scores. */
static void line48(const uint8_t *columns)
{
	ace_sta(WSYNC);
	ace_write(AUDV0, *sample++);  /* 5 */
	ace_write(GRP0, columns[0]);  /* 10 */
	ace_write(GRP1, columns[1]);  /* 15 */
	ace_write(GRP0, columns[2]);  /* 20 */
	ace_lda(columns[3]);          /* 22 */
	ldx(columns[4]);          /* 24 */
	ldy(columns[5]);          /* 26 */
	ace_jmp();                    /* 29 */
	ace_nops(5);                  /* 39 */
	ace_jmp();                    /* 42 */
	ace_sta(GRP1);                /* 45: column 4, column 3 shows */
	stx(GRP0);                /* 48: column 5, column 4 shows */
	sty(GRP1);                /* 51: column 6, column 5 shows */
	ace_sta(GRP0);                /* 54: column 6 shows */
	ace_jmp();
}

static void no_players(void)
{
	ace_sta(WSYNC);
	ace_write(AUDV0, *sample++);
	ace_write(GRP0, 0);
	ace_write(GRP1, 0);
	ace_write(GRP0, 0);           /* the delayed GRP0 too */
	ace_jmp();
}

/* A row of the cartridge's line: an asymmetric playfield, the right half's registers
 * written again as the beam passes the middle (PF0 from cycle 28 to 49, PF1 from 39 to 54,
 * PF2 from 50 to 65). */
static void cart_line(int row, int sides)
{
	ace_sta(WSYNC);
	ace_write(COLUPF, title.cart_colour[row]);  /* 5 */
	ace_write(PF0, cart_pf0[row]);              /* 10 */
	ace_write(PF1, cart_pf1[row]);              /* 15 */
	ace_write(PF2, cart_pf2[row]);              /* 20 */
	ace_write(AUDV0, *sample++);                /* 25 */
	ace_write(PF0, cart_pf0r[row]);             /* 30 */
	ace_nops(3);                                /* 36 */
	ace_write(PF1, cart_pf1r[row]);             /* 41 */
	ace_nops(2);                                /* 45 */
	ace_write(PF2, cart_pf2r[row]);             /* 50 */
	if (sides) {                                /* the label's sides from the next line on */
		ace_write(ENAM0, 2);                    /* 55 */
		ace_write(ENAM1, 2);                    /* 60 */
	}
	ace_jmp();
}

/* The title screen: 10 lines to set it up (the objects' places), VCSIDE's cartridge (90),
 * THE GAME (14) and the menu (10 lines a text), centred on PAL's 50 more lines. */
static void title_kernel(void)
{
	int line = 0, top = (lines - 199) / 2;
	sample = music_level;
	ace_overblank_end();
	ace_sta(WSYNC);
	ace_write(AUDV0, *sample++);
	ace_write(NUSIZ0, 0x20);      /* the missiles 4 pixels wide, one copy */
	ace_write(NUSIZ1, 0x20);
	ace_write(VDELP0, 1);
	ace_write(VDELP1, 1);
	ace_write(REFP0, 0);
	ace_write(CTRLPF, CART_CTRLPF);
	ace_write(COLUP0, title.sides_colour);
	ace_write(COLUP1, title.sides_colour);
	ace_write(HMP0, places[0].fine);
	ace_write(HMP1, places[1].fine);
	ace_write(HMM0, places[2].fine);
	ace_jmp();
	ace_write(HMM1, places[3].fine);
	position_line(RESP0, &places[0]);
	position_line(RESP1, &places[1]);
	position_line(RESM0, &places[2]);
	position_line(RESM1, &places[3]);
	ace_sta(WSYNC);
	ace_sta(HMOVE);
	ace_write(AUDV0, *sample++);
	ace_jmp();
	line = 6;
	for (int i = 0; i < 4; i++, line++) {
		ace_sta(WSYNC);
		ace_write(AUDV0, *sample++);
		overblank_samples(i);
		if (i == 0) ace_write(VBLANK, 0);
		ace_jmp();
	}
	for (; line < 10 + top; line++) blank_line();
	for (int row = 0; row < CART_ROWS; row++)
		for (int i = 0; i < CART_LINES; i++, line++) cart_line(row, row == 0 && i == CART_LINES - 1);
	ace_sta(WSYNC);
	ace_write(AUDV0, *sample++);
	ace_write(ENAM0, 0);
	ace_write(ENAM1, 0);
	ace_write(PF0, 0);
	ace_write(PF1, 0);
	ace_write(PF2, 0);
	ace_write(COLUP0, title.title_colour);
	ace_write(COLUP1, title.title_colour);
	ace_write(NUSIZ0, 0x03);      /* the players for the texts: three close copies each */
	ace_write(NUSIZ1, 0x03);
	ace_jmp();
	line++;
	for (int i = 1; i < 8; i++, line++) blank_line();
	for (int i = 0; i < TITLE_LINES; i++, line++) line48(screen_the_game[i / 2]);
	no_players();
	line++;
	for (int i = 1; i < 10; i++, line++) blank_line();
	for (int text = 0; text < title.texts; text++) {
		ace_sta(WSYNC);
		ace_write(AUDV0, *sample++);
		ace_write(COLUP0, title.text_colour[text]);
		ace_write(COLUP1, title.text_colour[text]);
		ace_jmp();
		line++;
		for (int i = 0; i < TEXT_LINES; i++, line++) line48(title.text[text][i]);
		no_players();
		line++;
		blank_line();
		line++;
	}
	for (; line < lines - 1; line++) blank_line();
	last_line();
}

/* ---- The game -------------------------------------------------------------------------- */

/* Player 1 moves to the lower enemy between two pairs of the room. The first line of the
 * band pair sets HMP1; its second line clears GRP1 and hits RESP1 at the right cycle; the
 * next pair's first line ends with an HMOVE at cycle 73, which leaves no black comb on the
 * left (the HMOVE at the start of a line does). That late HMOVE moves an object 8 pixels
 * further left than HMxx says, or 7 on the consoles with the short HMOVE (2600s made in
 * China from 1989 on, the late Jr. and every PAL 7800: ZackAttack's measurements and
 * Eckhard Stolberg's note, AtariAge 2018), which short_hmove = 1 makes up for (the ELF template's menu has an HMOVE row for it).
 * The other objects get HMxx = "still" for it. */
static int band_cycle;            /* where RESP1's write ends */
static uint8_t band_hmp1, still;

/* Worked out in the overblank: RESP1 can end at cycle 25 at the earliest (after the line's
 * four writes and a wait) and puts player 1 at 3 x cycle - 63, 0 to 14 pixels to the right
 * of x; the late HMOVE takes it back. */
static void band_timing(int x)
{
	int cycle = (x + 63 + 2) / 3;
	if (cycle < 25) cycle = 25;
	int left = 3 * cycle - 63 - x;            /* 0-14 pixels to the left */
	band_cycle = cycle;
	band_hmp1 = (uint8_t)(((left - 8 + short_hmove) & 0x0F) << 4);
}

/* A wait of cycles (2 or more) with nops and a jmp. */
static void wait_cycles(int cycles)
{
	if (cycles & 1) {
		ace_jmp();
		cycles -= 3;
	}
	if (cycles) ace_nops(cycles / 2);
}

/* The game: the objects' places and the overblank's samples (9 lines), the score (12),
 * the room (176), centred on PAL's 50 more lines. */
static void game_kernel(void)
{
	int line, top = (lines - 199) / 2;
	sample = music_level;
	ace_overblank_end();
	ace_sta(WSYNC);
	ace_write(AUDV0, *sample++);
	ace_write(NUSIZ0, 0x20);      /* missile 0 four pixels wide, one copy of each player */
	ace_write(NUSIZ1, 0);
	ace_write(VDELP0, 0);         /* (the title's text uses them) */
	ace_write(VDELP1, 0);
	ace_write(REFP0, screen.refp0);
	ace_write(HMP0, places[0].fine);
	ace_write(HMP1, places[1].fine);
	ace_write(HMM0, places[2].fine);
	ace_jmp();
	position_line(RESP0, &places[0]);
	position_line(RESP1, &places[1]);
	position_line(RESM0, &places[2]);
	ace_sta(WSYNC);
	ace_sta(HMOVE);
	ace_write(AUDV0, *sample++);
	ace_write(CTRLPF, 0x02);      /* score mode: the left half in COLUP0, the right in COLUP1 */
	ace_write(COLUP0, screen.score_colour);
	ace_write(COLUP1, screen.lives_colour);
	ace_write(COLUPF, screen.wall_colour);
	ace_jmp();
	for (int i = 0; i < 4; i++) {
		ace_sta(WSYNC);
		ace_write(AUDV0, *sample++);
		overblank_samples(i);
		if (i == 0) {             /* for the late HMOVE: nothing moves but player 1 */
			ace_write(HMP0, still);
			ace_write(HMM0, still);
			ace_write(VBLANK, 0);
		}
		ace_jmp();
	}
	line = 9;
	for (; line < 9 + top; line++) blank_line();
	/* the score, two lines a row: PF1 on the left, and on the right (cycles 39 to 54) */
	blank_line();
	line++;
	for (int row = 0; row < SCORE_ROWS; row++)
		for (int i = 0; i < 2; i++, line++) {
			ace_sta(WSYNC);
			ace_write(AUDV0, *sample++);              /* 5 */
			ace_write(PF1, screen.score_pf1[row]);    /* 10 */
			ace_jmp();                                /* 13 */
			ace_nops(12);                             /* 37 */
			ace_write(PF1, screen.lives_pf1[row]);    /* 42 */
			ace_jmp();
		}
	ace_sta(WSYNC);
	ace_write(AUDV0, *sample++);
	ace_write(PF1, 0);
	ace_write(CTRLPF, ROOM_CTRLPF);
	ace_jmp();
	line++;
	/* the room, a line pair at a time */
	int band = screen.band_pair, late = band >= 0 ? band + 1 : -1;
	for (int pair = 0; pair < ROOM_PAIRS; pair++, line += 2) {
		int row = 2 * pair / ROOM_LINES;
		ace_sta(WSYNC);
		ace_write(PF0, room_pf0[row]);                /* 5 */
		ace_write(PF1, room_pf1[row]);                /* 10 */
		ace_write(PF2, room_pf2[row]);                /* 15 */
		ace_write(ENAM0, screen.enam0[pair]);         /* 20 */
		ace_write(AUDV0, *sample++);                  /* 25 */
		if (pair == band) ace_write(HMP1, band_hmp1); /* 30 */
		if (pair == late) {
			wait_cycles(70 - 25);
			ace_sta(HMOVE);                           /* 73: the late HMOVE */
		} else {
			ace_jmp();
		}
		ace_sta(WSYNC);
		if (pair == band) {                           /* player 1 to the lower enemy */
			ace_write(GRP1, 0);                       /* 5 */
			ace_write(GRP0, screen.grp0[pair]);       /* 10 */
			ace_write(COLUP0, screen.colup0[pair]);   /* 15 */
			ace_write(AUDV0, *sample++);              /* 20 */
			wait_cycles(band_cycle - 3 - 20);
			ace_sta(RESP1);                           /* band_cycle */
			if (band_cycle <= 70) ace_jmp();          /* (later, the next WSYNC would miss the line) */
		} else {
			ace_write(GRP0, screen.grp0[pair]);       /* 5 */
			ace_write(COLUP0, screen.colup0[pair]);   /* 10 */
			ace_write(GRP1, screen.grp1[pair]);       /* 15 */
			ace_write(COLUP1, screen.colup1[pair]);   /* 20 */
			ace_write(AUDV0, *sample++);              /* 25 */
			ace_jmp();
		}
	}
	ace_sta(WSYNC);
	ace_write(PF0, 0);
	ace_write(PF1, 0);
	ace_write(PF2, 0);
	ace_write(GRP0, 0);
	ace_write(GRP1, 0);
	ace_write(ENAM0, 0);
	ace_write(AUDV0, *sample++);
	ace_jmp();
	line++;
	for (; line < lines - 1; line++) blank_line();
	last_line();
}

/* ---- main ------------------------------------------------------------------------------ */

int main(void)
{
	/* ACE passes no TV system (the SYSTEM row starts at the project's TV standard) and no
	 * flags: EXIT is always there, and the menu has no HMOVE row. The ARM's clock is in
	 * ACE_ARGUMENTS->clock_hz; the frame is timed by the 6507's lines, so it is not used. */
	menu_init(&menu, DEFAULT_SYSTEM, 0, 0, 1);
	music_init();
	start_6507();
	int playing = 0;
	uint8_t last_inpt4 = 0x80, last_swchb = 0xFF;
	for (;;) {
		/* Overblank: the game logic. The button counts when it goes down. */
		int fire = !(inpt4 & 0x80) && (last_inpt4 & 0x80);
		int reset = !(swchb & 0x01) && (last_swchb & 0x01);
		last_inpt4 = inpt4;
		last_swchb = swchb;
		if (!playing) {
			int chosen = menu_frame(&menu, swcha, fire);
			if (chosen == MENU_PLAY) {
				game_start(&game);
				playing = 1;
			} else if (chosen == MENU_EXIT) {
				leave();
				return 1;         /* the start code hands the bus back to the firmware */
			}
		} else {
			int events = game_frame(&game, swcha, fire);
			if (events & GAME_SHOT) music_effect(MUSIC_EFFECT_SHOT);
			if (events & GAME_HIT) music_effect(MUSIC_EFFECT_HIT);
			if (events & GAME_LOSE) music_effect(MUSIC_EFFECT_LOSE);
			if ((events & GAME_OVER) || reset) {     /* back to the title, on PLAY */
				playing = 0;
				menu.row = 0;
			}
		}
		short_hmove = menu.short_hmove;
		lines = menu.system == SYSTEM_PAL ? 312 - 63 : 262 - 63;
		if (playing) {
			screen_game(&screen, &game, menu.system);
			place(&places[0], screen.hero_x);
			place(&places[1], screen.enemy_x);
			place(&places[2], screen.shot_x + 1);
			still = (uint8_t)(0x80 + (short_hmove << 4));
			if (screen.band_pair >= 0) band_timing(screen.band_x);
		} else {
			screen_title(&title, &menu);
			place(&places[0], TITLE_P0_X);
			place(&places[1], TITLE_P1_X);
			place(&places[2], CART_LEFT_SIDE + 1);
			place(&places[3], CART_RIGHT_SIDE + 1);
		}
		/* the frame's music: samples for the kernel's lines and for the overblank's */
		music_frame(lines + MUSIC_OVERBLANK_LINES);
		if (playing) game_kernel();
		else title_kernel();
	}
}
