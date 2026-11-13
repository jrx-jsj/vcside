/* {{NAME}} - VCSIDE The Game's logic, run by the cartridge's ARM
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * main.asm calls game_arm() through CALLFN (start.S is the way in): once at power on, then
 * at the start of every frame. They talk through the display data (layout.h): the 6507
 * leaves the joystick, the console switches, the TV system and the collisions of the frame
 * it drew last; this code runs the frame and leaves what the 6507 writes to the TIA
 * (send_registers, in the order main.asm reads them), and the data streams the kernels
 * read, a byte per line (title_streams, game_streams).
 *
 * The game: a room with walls where the hero walks and shoots at two enemies. Hitting one
 * scores a point; touching one costs a life. The title's menu: up and down choose PLAY or
 * the TV system, left, right and the button change the system, the button on PLAY starts.
 *
 * Graphics (open them from the project panel; saving one writes its .gfx.h): hero.vcsgfx,
 * enemy.vcsgfx, room.vcsgfx (22 rows of 8 lines, mirrored) and title.vcsgfx (the title's
 * cartridge and the menu's texts). THE GAME is written below (the_game).
 * The music: theme.vcsmusic is the song, played as samples with three voices, and three
 * effects: shot (takes the third voice), hit (on the TIA's channel 1) and lose (the third
 * voice); saving it writes music.h. The cartridge plays the voices: each one's
 * waveform (32 samples) in the display data, its note set with the driver.
 */
#include <stdint.h>

#include "cdfj.h"
#include "layout.h"
#include "hero.gfx.h"
#include "enemy.gfx.h"
#include "room.gfx.h"
#include "title.gfx.h"

static void voice_waveform(int voice, const uint8_t *samples);
static void voice_note(int voice, uint32_t frequency, int restart);
#define MUSIC_CDFJ_WAVE(voice, samples) voice_waveform(voice, samples)
#define MUSIC_CDFJ_NOTE(voice, frequency, restart) voice_note(voice, frequency, restart)
#include "music.h"

#define LIVES 3
#define ROOM_PAIRS (ROOM_ROWS * ROOM_LINES / 2)    /* 88 line pairs of room */
#define HERO_START_X 76
#define HERO_START_Y 8                              /* line pair of the hero's bottom row */
#define SHOT_SPEED 3
#define CART_ALL_LINES (CART_ROWS * CART_LINES)     /* the title's cartridge: 90 lines */
#define TEXT48_LINES 14                             /* THE GAME */
#define TEXT_LINES (PLAY_ROWS * PLAY_LINES)         /* each menu text: 15 lines */
#define NO_ENEMY 0xF0                               /* enemy_row when none is drawn */

enum { SYSTEM_NTSC, SYSTEM_PAL60, SYSTEM_PAL };
enum { MODE_TITLE, MODE_GAME };

/* The streams' data in the display data. The title's and the game's take the same place:
 * a frame fills only the ones its kernel reads. */
enum {
	BUFFER_CART = LAYOUT_BUFFERS,                       /* 8 x CART_ALL_LINES: STREAM_CART to _SIDES */
	BUFFER_THE_GAME = BUFFER_CART + 8 * CART_ALL_LINES, /* 6 x TEXT48_LINES */
	BUFFER_PLAY = BUFFER_THE_GAME + 6 * TEXT48_LINES,   /* 6 x TEXT_LINES */
	BUFFER_SYSTEM = BUFFER_PLAY + 6 * TEXT_LINES,       /* 6 x TEXT_LINES */
	BUFFER_TITLE_END = BUFFER_SYSTEM + 6 * TEXT_LINES,
	BUFFER_ROOM = LAYOUT_BUFFERS,                       /* 3 x ROOM_PAIRS: PF0, PF1, PF2 */
	BUFFER_SPRITES = BUFFER_ROOM + 3 * ROOM_PAIRS,      /* 4 x ROOM_PAIRS: GRP0, COLUP0, GRP1, ENABL */
	BUFFER_SCORE = BUFFER_SPRITES + 4 * ROOM_PAIRS,     /* 10 lines: the score, then 10: the lives */
	BUFFER_GAME_END = BUFFER_SCORE + 20,
};
_Static_assert(BUFFER_TITLE_END <= 4096 && BUFFER_GAME_END <= 4096, "the display data is 4K");
_Static_assert(LAYOUT_OUT + 23 <= LAYOUT_WAVES, "what the 6507 reads must end before the waveforms");

/* THE GAME, 48 pixels wide: six columns of 8, a line each, top first. */
static const uint8_t the_game[TEXT48_LINES][6] = {
	{0xFA, 0x2F, 0x80, 0x79, 0xC8, 0xBE},  /* XXXXX.X...X.XXXXX........XXXX..XXX..X...X.XXXXX. */
	{0xFA, 0x2F, 0x80, 0x79, 0xC8, 0xBE},  /* XXXXX.X...X.XXXXX........XXXX..XXX..X...X.XXXXX. */
	{0x22, 0x28, 0x00, 0x82, 0x2D, 0xA0},  /* ..X...X...X.X...........X.....X...X.XX.XX.X..... */
	{0x22, 0x28, 0x00, 0x82, 0x2D, 0xA0},  /* ..X...X...X.X...........X.....X...X.XX.XX.X..... */
	{0x22, 0x28, 0x00, 0x82, 0x2A, 0xA0},  /* ..X...X...X.X...........X.....X...X.X.X.X.X..... */
	{0x22, 0x28, 0x00, 0x82, 0x2A, 0xA0},  /* ..X...X...X.X...........X.....X...X.X.X.X.X..... */
	{0x23, 0xEF, 0x00, 0x9B, 0xEA, 0xBC},  /* ..X...XXXXX.XXXX........X..XX.XXXXX.X.X.X.XXXX.. */
	{0x23, 0xEF, 0x00, 0x9B, 0xEA, 0xBC},  /* ..X...XXXXX.XXXX........X..XX.XXXXX.X.X.X.XXXX.. */
	{0x22, 0x28, 0x00, 0x8A, 0x28, 0xA0},  /* ..X...X...X.X...........X...X.X...X.X...X.X..... */
	{0x22, 0x28, 0x00, 0x8A, 0x28, 0xA0},  /* ..X...X...X.X...........X...X.X...X.X...X.X..... */
	{0x22, 0x28, 0x00, 0x8A, 0x28, 0xA0},  /* ..X...X...X.X...........X...X.X...X.X...X.X..... */
	{0x22, 0x28, 0x00, 0x8A, 0x28, 0xA0},  /* ..X...X...X.X...........X...X.X...X.X...X.X..... */
	{0x22, 0x2F, 0x80, 0x7A, 0x28, 0xBE},  /* ..X...X...X.XXXXX........XXXX.X...X.X...X.XXXXX. */
	{0x22, 0x2F, 0x80, 0x7A, 0x28, 0xBE},  /* ..X...X...X.XXXXX........XXXX.X...X.X...X.XXXXX. */
};

/* The menu's texts of title.vcsgfx: PLAY, then the systems' names, by System. */
static const uint8_t *const menu_texts[4][6] = {
	{play_pf0, play_pf1, play_pf2, play_pf0r, play_pf1r, play_pf2r},
	{ntsc_pf0, ntsc_pf1, ntsc_pf2, ntsc_pf0r, ntsc_pf1r, ntsc_pf2r},
	{pal60_pf0, pal60_pf1, pal60_pf2, pal60_pf0r, pal60_pf1r, pal60_pf2r},
	{pal_pf0, pal_pf1, pal_pf2, pal_pf0r, pal_pf1r, pal_pf2r},
};

/* The digits 0-9, five rows each, top first, in both halves of the byte (PF1). */
static const uint8_t digits[10][5] = {
	{0xEE, 0xAA, 0xAA, 0xAA, 0xEE}, {0x22, 0x22, 0x22, 0x22, 0x22}, {0xEE, 0x22, 0xEE, 0x88, 0xEE},
	{0xEE, 0x22, 0x66, 0x22, 0xEE}, {0xAA, 0xAA, 0xEE, 0x22, 0x22}, {0xEE, 0x88, 0xEE, 0x22, 0xEE},
	{0xEE, 0x88, 0xEE, 0xAA, 0xEE}, {0xEE, 0x22, 0x22, 0x22, 0x22}, {0xEE, 0xAA, 0xEE, 0xAA, 0xEE},
	{0xEE, 0xAA, 0xEE, 0x22, 0xEE},
};

/* The closest PAL colour to each NTSC one, $00 to $FE in steps of 2: worked out from
 * Stella's NTSC and PAL palettes, as the shortest distance in CIELAB (the colour space
 * closest to how the eye tells colours apart). Change an entry to taste. */
static const uint8_t ntsc_to_pal[128] = {
	0x00, 0x04, 0x06, 0x08, 0x0A, 0x0A, 0x0C, 0x0E, /* $00: grey */
	0x22, 0x24, 0x24, 0x28, 0x28, 0x28, 0x2A, 0x2C, /* $10: gold */
	0x42, 0x44, 0x46, 0x46, 0x46, 0x28, 0x2A, 0x2A, /* $20: orange */
	0x42, 0x42, 0x44, 0x46, 0x46, 0x48, 0x48, 0x4A, /* $30: red orange */
	0x42, 0x62, 0x62, 0x64, 0x64, 0x66, 0x68, 0x68, /* $40: pink */
	0x82, 0x82, 0x84, 0x84, 0x86, 0x86, 0x88, 0x88, /* $50: purple */
	0xA0, 0xA2, 0xA4, 0xA6, 0xA6, 0xA8, 0xA8, 0xA8, /* $60: blue purple */
	0xC0, 0xC0, 0xD4, 0xD4, 0xD6, 0xC8, 0xC8, 0xCA, /* $70: blue */
	0xD0, 0xD2, 0xD4, 0xD4, 0xD6, 0xD6, 0xD8, 0xD8, /* $80: blue */
	0xB2, 0xB2, 0xB4, 0xB6, 0xB6, 0xB8, 0xBA, 0xBA, /* $90: light blue */
	0x90, 0x92, 0x92, 0x94, 0x96, 0x98, 0x9A, 0x9A, /* $A0: turquoise */
	0x70, 0x72, 0x72, 0x76, 0x78, 0x78, 0x7A, 0x7C, /* $B0: green blue */
	0x30, 0x50, 0x32, 0x56, 0x58, 0x58, 0x5A, 0x5A, /* $C0: green */
	0x30, 0x50, 0x32, 0x56, 0x58, 0x3A, 0x3C, 0x3C, /* $D0: yellow green */
	0x20, 0x22, 0x24, 0x24, 0x2A, 0x2A, 0x2A, 0x2C, /* $E0: orange green */
	0x20, 0x22, 0x24, 0x24, 0x28, 0x2A, 0x2A, 0x2C, /* $F0: light orange */
};

/* The variables: all zero at power on (start), none with an initial value (arm.ld). */
static uint8_t tv_system, mode, drawn_mode, menu_row, last_stick, button, frame_count, random;
static uint8_t hero_x, hero_y, old_hero_x, old_hero_y, facing;   /* facing: REFP0, 0 right or 8 left */
static vcsgfx_player hero_animation;
static uint8_t hero_frame;
static uint8_t enemy_x[2], enemy_y[2], enemy_live, shown, old_enemy_x, old_enemy_y, enemy_row;
static int8_t enemy_dx[2], enemy_dy[2];
static uint8_t shot_x, shot_pair, score, lives;                  /* shot_pair 0: no shot */
static int8_t shot_dx;
static uint8_t *out;                                             /* send_registers' next byte */

/* ---- the music ---- */

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

/* ---- small things ---- */

/* An NTSC colour, for the TV system: PAL and PAL60 take the closest PAL one. */
static uint8_t tv_colour(uint8_t colour) { return tv_system == SYSTEM_NTSC ? colour : ntsc_to_pal[colour >> 1]; }

/* The joystick directions pressed on this frame and not on the last one, as 1 bits (D7
 * right, D6 left, D5 down, D4 up). */
static uint8_t stick_presses(uint8_t swcha)
{
	uint8_t pressed = (uint8_t)~swcha & last_stick & 0xF0;
	last_stick = swcha;
	return pressed;
}

/* Whether the fire button has just gone down (bit 7 of INPT4: 0 down). */
static int button_press(uint8_t inpt4)
{
	int pressed = !(inpt4 & 0x80) && (button & 0x80);
	button = inpt4 & 0x80;
	return pressed;
}

/* ---- the game ---- */

static void place_hero(void)
{
	hero_x = old_hero_x = HERO_START_X;
	hero_y = old_hero_y = HERO_START_Y;
}

/* Both enemies back, near the top of the room. */
static void new_wave(void)
{
	for (int i = 0; i < 2; i++) {
		enemy_x[i] = i ? 112 : 40;
		enemy_y[i] = 70;
		enemy_dx[i] = i ? -1 : 1;
		enemy_dy[i] = -1;                   /* down */
	}
	enemy_live = 3;
}

static void start_game(void)
{
	mode = MODE_GAME;
	lives = LIVES;
	score = 0;
	shot_pair = 0;
	shown = 0;
	enemy_row = NO_ENEMY;
	place_hero();
	new_wave();
}

/* The joystick moves the hero one clock (or one line pair) a frame and chooses its
 * animation and the way it faces. */
static void move_hero(uint8_t swcha)
{
	int moving = 0;
	old_hero_x = hero_x;
	old_hero_y = hero_y;
	if (!(swcha & 0x80)) {                  /* right */
		moving = 1;
		facing = 0;
		if (hero_x < 151) hero_x++;
	}
	if (!(swcha & 0x40)) {                  /* left */
		moving = 1;
		facing = 8;
		if (hero_x >= 2) hero_x--;
	}
	if (!(swcha & 0x10)) {                  /* up */
		moving = 1;
		if (hero_y < ROOM_PAIRS - HERO_P0_HEIGHT) hero_y++;
	}
	if (!(swcha & 0x20)) {                  /* down */
		moving = 1;
		if (hero_y >= 2) hero_y--;
	}
	vcsgfx_play(&hero_animation, moving ? &hero_walk : &hero_idle);
	hero_frame = (uint8_t)vcsgfx_animate(&hero_animation);
}

/* The button fires one shot the way the hero faces; it flies until it hits something or
 * leaves the screen. */
static void shoot(uint8_t inpt4)
{
	if (button_press(inpt4) && !shot_pair) {
		shot_pair = hero_y + HERO_P0_HEIGHT / 2;
		shot_x = facing ? hero_x - 2 : hero_x + 8;  /* from the hero's left or right side */
		shot_dx = facing ? -SHOT_SPEED : SHOT_SPEED;
		music_effect(MUSIC_EFFECT_SHOT);
	}
	if (shot_pair) {
		shot_x += shot_dx;
		if (shot_x >= 158) shot_pair = 0;   /* off the screen (on the left it wraps above 158) */
	}
}

/* Each enemy moves diagonally on every other frame (they take turns, 30 Hz each); the
 * collisions turn it round. Both alive: they share player 1 and take turns on the screen
 * too (30 Hz flicker). One alive: it is drawn on every frame. */
static void move_enemy(void)
{
	int turn = frame_count & 1;             /* whose turn it is to move */
	enemy_row = NO_ENEMY;
	shown = enemy_live == 1 ? 0 : enemy_live == 2 ? 1 : turn;
	if (!(enemy_live & (1 << shown))) return;
	old_enemy_x = enemy_x[shown];           /* where it was, for the collisions */
	old_enemy_y = enemy_y[shown];
	if (shown == turn) {
		enemy_x[shown] += enemy_dx[shown];
		enemy_y[shown] += enemy_dy[shown];
	}
	enemy_row = enemy_y[shown];
}

/* What touched what on the frame just drawn (the TIA's collision registers). */
static void collisions(const uint8_t *display)
{
	if (display[LAYOUT_CXP0FB] & 0x80) {    /* the hero and the playfield: back where it was */
		hero_x = old_hero_x;
		hero_y = old_hero_y;
	}
	if (enemy_row != NO_ENEMY && (display[LAYOUT_CXP1FB] & 0x80)) {
		/* the enemy and the playfield: back where it was, and the other way on one axis.
		 * The TIA does not say which side it touched, so the axis is chosen at random; a
		 * wrong guess touches again and turns the other one (a fixed rule traps them). */
		enemy_x[shown] = old_enemy_x;
		enemy_y[shown] = old_enemy_y;
		if (random & 1) enemy_dx[shown] = -enemy_dx[shown];
		else enemy_dy[shown] = -enemy_dy[shown];
	}
	if (display[LAYOUT_CXBLPF] & 0x80) shot_pair = 0;  /* the shot and the playfield */
	if ((display[LAYOUT_CXP1FB] & 0x40) && shot_pair) { /* the shot and the enemy */
		shot_pair = 0;
		enemy_live &= ~(1 << shown);
		score = score == 99 ? 0 : score + 1;
		music_effect(MUSIC_EFFECT_HIT);
		if (!enemy_live) new_wave();        /* both gone: two more */
	}
	if (display[LAYOUT_CXPPMM] & 0x80) {    /* the hero and the enemy */
		music_effect(MUSIC_EFFECT_LOSE);
		place_hero();
		new_wave();                         /* the enemies back at the top, away from the hero */
		if (--lives == 0) mode = MODE_TITLE;
	}
}

/* The menu. Up and down choose the row, left and right change the TV system; the button
 * on PLAY starts the game, on the TV row it changes the system. */
static void title_logic(uint8_t swcha, uint8_t inpt4)
{
	uint8_t pressed = stick_presses(swcha);
	if (pressed & 0x30) menu_row ^= 1;
	if (menu_row == 0) {
		if (button_press(inpt4)) start_game();
		return;
	}
	if (pressed & 0x80) tv_system = tv_system == SYSTEM_PAL ? SYSTEM_NTSC : tv_system + 1;
	if (pressed & 0x40) tv_system = tv_system == SYSTEM_NTSC ? SYSTEM_PAL : tv_system - 1;
	if (button_press(inpt4)) tv_system = tv_system == SYSTEM_PAL ? SYSTEM_NTSC : tv_system + 1;
}

/* ---- what the 6507 draws ---- */

static void send(uint8_t value) { *out++ = value; }

/* The frame's registers, in the order main.asm reads them after CallArm. */
static void send_registers(void)
{
	out = CDFJ_DISPLAY + LAYOUT_OUT;
	send(mode);
	send(tv_system);
	if (mode == MODE_TITLE) {
		send(52);                           /* P0 and P1: THE GAME, on pixels 55 and 63 */
		send(60);
		send(34);                           /* M0 and M1: the label's sides, pixels 36 and 116 */
		send(114);
		send(0);                            /* the ball: not used */
		send(0);                            /* REFP0 */
		send(0x20);                         /* NUSIZ0 and NUSIZ1: missiles 4 clocks wide */
		send(0x20);
		send(1);                            /* VDELP0 and VDELP1: for THE GAME */
		send(1);
		send(0);                            /* CTRLPF: the texts are not mirrored */
		send(0);                            /* COLUPF (each row has its own) */
		send(0);                            /* COLUBK */
		send(tv_colour(0x06));              /* COLUP0 and COLUP1: the sides' grey */
		send(tv_colour(0x06));
	} else {
		send(hero_x);                       /* P0 */
		send(enemy_x[shown]);               /* P1 */
		send(0);                            /* M0 and M1: not used */
		send(0);
		send(shot_x);                       /* the ball: the shot */
		send(facing);                       /* REFP0 */
		send(0);                            /* NUSIZ0 and NUSIZ1 */
		send(0);
		send(0);                            /* VDELP0 and VDELP1 */
		send(0);
		send(0x02);                         /* CTRLPF: the score in COLUP0 and COLUP1 */
		send(tv_colour(ROOM_COLUPF));       /* COLUPF: the walls and the shot */
		send(tv_colour(ROOM_COLUBK));       /* COLUBK */
		send(0x0E);                         /* COLUP0: the score */
		send(tv_colour(0x1C));              /* COLUP1: the lives */
	}
	for (int i = 0; i < 3; i++) send(music_tia[i]);  /* AUDC1, AUDF1, AUDV1 */
}

/* The title: the cartridge, THE GAME, PLAY and the TV system, a byte per line. */
static void title_streams(void)
{
	uint8_t *display = CDFJ_DISPLAY;
	for (int line = 0; line < CART_ALL_LINES; line++) {
		int row = line / CART_LINES;
		const uint8_t *tables[6] = {cart_pf0, cart_pf1, cart_pf2, cart_pf0r, cart_pf1r, cart_pf2r};
		for (int i = 0; i < 6; i++) display[BUFFER_CART + i * CART_ALL_LINES + line] = tables[i][row];
		display[BUFFER_CART + 6 * CART_ALL_LINES + line] = tv_colour(cart_colupf[row]);
		display[BUFFER_CART + 7 * CART_ALL_LINES + line] = row ? 2 : 0;  /* the sides, after the lip */
	}
	for (int line = 0; line < TEXT48_LINES; line++)
		for (int i = 0; i < 6; i++) display[BUFFER_THE_GAME + i * TEXT48_LINES + line] = the_game[line][i];
	for (int line = 0; line < TEXT_LINES; line++)
		for (int i = 0; i < 6; i++) {
			display[BUFFER_PLAY + i * TEXT_LINES + line] = menu_texts[0][i][line / PLAY_LINES];
			display[BUFFER_SYSTEM + i * TEXT_LINES + line] = menu_texts[1 + tv_system][i][line / PLAY_LINES];
		}
	for (int i = 0; i < 8; i++) cdfj_point(STREAM_CART + i, BUFFER_CART + i * CART_ALL_LINES);
	for (int i = 0; i < 6; i++) {
		cdfj_point(STREAM_THE_GAME + i, BUFFER_THE_GAME + i * TEXT48_LINES);
		cdfj_point(STREAM_PLAY + i, BUFFER_PLAY + i * TEXT_LINES);
		cdfj_point(STREAM_SYSTEM + i, BUFFER_SYSTEM + i * TEXT_LINES);
	}
	for (int stream = STREAM_CART; stream < STREAM_SYSTEM + 6; stream++) cdfj_step(stream, 1, 0);
	send(tv_colour(0x9C));                  /* THE GAME: light blue */
	send(menu_row == 0 ? 0x0E : 0x06);      /* PLAY: white when selected, else grey */
	send(menu_row == 1 ? 0x0E : 0x06);      /* the TV system */
}

/* The game: the room, the hero, the enemy and the shot, a byte per line pair from the top
 * (pass k shows line pair ROOM_PAIRS - k), and the score. */
static void game_streams(void)
{
	uint8_t *display = CDFJ_DISPLAY;
	uint8_t hero_colours[HERO_P0_HEIGHT];
	for (int row = 0; row < HERO_P0_HEIGHT; row++) hero_colours[row] = tv_colour(hero_P0_colour[hero_frame][row]);
	int enemy_frame = (frame_count >> 4) & 1;  /* its two frames, 16 frames each */
	for (int pass = 0; pass < ROOM_PAIRS; pass++) {
		int pair = ROOM_PAIRS - pass, row = pass / (ROOM_LINES / 2);
		display[BUFFER_ROOM + pass] = room_pf0[row];
		display[BUFFER_ROOM + ROOM_PAIRS + pass] = room_pf1[row];
		display[BUFFER_ROOM + 2 * ROOM_PAIRS + pass] = room_pf2[row];
		int hero_row = hero_y + HERO_P0_HEIGHT - 1 - pair;  /* top first */
		int inside = hero_row >= 0 && hero_row < HERO_P0_HEIGHT;
		display[BUFFER_SPRITES + pass] = inside ? hero_P0_gfx[hero_frame][hero_row] : 0;
		display[BUFFER_SPRITES + ROOM_PAIRS + pass] = inside ? hero_colours[hero_row] : 0;
		int enemy_line = enemy_row + ENEMY_P1_HEIGHT - 1 - pair;
		inside = enemy_row != NO_ENEMY && enemy_line >= 0 && enemy_line < ENEMY_P1_HEIGHT;
		display[BUFFER_SPRITES + 2 * ROOM_PAIRS + pass] = inside ? enemy_P1_gfx[enemy_frame][enemy_line] : 0;
		display[BUFFER_SPRITES + 3 * ROOM_PAIRS + pass] = pair == shot_pair ? 2 : 0;
	}
	/* the score's two digits on the left half of PF1, the lives' digit on the right one,
	 * two lines per row */
	for (int line = 0; line < 10; line++) {
		int row = line >> 1;
		display[BUFFER_SCORE + line] = (digits[score / 10][row] & 0xF0) | (digits[score % 10][row] & 0x0F);
		display[BUFFER_SCORE + 10 + line] = digits[lives][row] & 0x0F;
	}
	for (int i = 0; i < 3; i++) cdfj_point(STREAM_ROOM + i, BUFFER_ROOM + i * ROOM_PAIRS);
	cdfj_point(STREAM_HERO, BUFFER_SPRITES);
	cdfj_point(STREAM_HERO_COLOUR, BUFFER_SPRITES + ROOM_PAIRS);
	cdfj_point(STREAM_ENEMY, BUFFER_SPRITES + 2 * ROOM_PAIRS);
	cdfj_point(STREAM_SHOT, BUFFER_SPRITES + 3 * ROOM_PAIRS);
	cdfj_point(STREAM_SCORE, BUFFER_SCORE);
	cdfj_point(STREAM_LIVES, BUFFER_SCORE + 10);
	for (int stream = STREAM_ROOM; stream <= STREAM_LIVES; stream++) cdfj_step(stream, 1, 0);
	send(ROOM_CTRLPF | 0x20);               /* the room's CTRLPF, and a ball 4 clocks wide */
	uint8_t enemy_colour = enemy_P1_colour[0][0];  /* the enemy's top row's colour... */
	if (shown) enemy_colour += 0x40;        /* ...the second one in another hue */
	send(tv_colour(enemy_colour));
}

/* ---- the way in ---- */

static void start(uint8_t system)
{
	for (uint8_t *byte = __bss_start; byte < __bss_end; byte++) *byte = 0;
	for (int voice = 0; voice < 3; voice++) cdfj_set_wave_size(voice, 27);
	music_init();
	tv_system = system;
	last_stick = 0xFF;
	button = 0x80;                          /* as if up: holding it at power on does nothing */
	random = 0xFF;                          /* any start but 0 */
}

static void frame(const uint8_t *display)
{
	tv_system = display[LAYOUT_SYSTEM];
	frame_count++;
	random = (random >> 1) ^ (random & 1 ? 0xB4 : 0);  /* an 8-bit Galois LFSR */
	if (drawn_mode == MODE_GAME && mode == MODE_GAME) collisions(display);
	if (mode == MODE_TITLE) title_logic(display[LAYOUT_SWCHA], display[LAYOUT_INPT4]);
	else if (!(display[LAYOUT_SWCHB] & 1)) mode = MODE_TITLE;  /* RESET: back to the title */
	else {
		move_hero(display[LAYOUT_SWCHA]);
		shoot(display[LAYOUT_INPT4]);
		move_enemy();
	}
	music_frame(tv_system == SYSTEM_PAL ? 312 : 262);
	send_registers();
	if (mode == MODE_TITLE) title_streams();
	else game_streams();
	drawn_mode = mode;
}

void game_arm(void)
{
	uint8_t *display = CDFJ_DISPLAY;
	if (display[LAYOUT_FUNCTION] == FUNCTION_START) start(display[LAYOUT_SYSTEM]);
	else frame(display);
	cdfj_step(CDFJ_COMM_STREAM, 1, 0);      /* the 6507 reads what follows with DSCOMM */
}
