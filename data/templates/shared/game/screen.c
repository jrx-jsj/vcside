/*
 * {{NAME}} - what the kernels draw (see screen.h).
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 */
#include "screen.h"

/* The closest PAL colour to each NTSC one, $00 to $FE in steps of 2: worked out from
 * Stella's NTSC and PAL palettes as the shortest distance in CIELAB (the colour space
 * closest to how the eye tells colours apart). PAL has no match for some NTSC colours,
 * like the brightest yellows: the closest one is a little off. Change an entry to taste. */
static const uint8_t ntsc_to_pal[128] = {
	0x00, 0x04, 0x06, 0x08, 0x0A, 0x0A, 0x0C, 0x0E,  /* $00: grey */
	0x22, 0x24, 0x24, 0x28, 0x28, 0x28, 0x2A, 0x2C,  /* $10: gold */
	0x42, 0x44, 0x46, 0x46, 0x46, 0x28, 0x2A, 0x2A,  /* $20: orange */
	0x42, 0x42, 0x44, 0x46, 0x46, 0x48, 0x48, 0x4A,  /* $30: red orange */
	0x42, 0x62, 0x62, 0x64, 0x64, 0x66, 0x68, 0x68,  /* $40: pink */
	0x82, 0x82, 0x84, 0x84, 0x86, 0x86, 0x88, 0x88,  /* $50: purple */
	0xA0, 0xA2, 0xA4, 0xA6, 0xA6, 0xA8, 0xA8, 0xA8,  /* $60: blue purple */
	0xC0, 0xC0, 0xD4, 0xD4, 0xD6, 0xC8, 0xC8, 0xCA,  /* $70: blue */
	0xD0, 0xD2, 0xD4, 0xD4, 0xD6, 0xD6, 0xD8, 0xD8,  /* $80: blue */
	0xB2, 0xB2, 0xB4, 0xB6, 0xB6, 0xB8, 0xBA, 0xBA,  /* $90: light blue */
	0x90, 0x92, 0x92, 0x94, 0x96, 0x98, 0x9A, 0x9A,  /* $A0: turquoise */
	0x70, 0x72, 0x72, 0x76, 0x78, 0x78, 0x7A, 0x7C,  /* $B0: green blue */
	0x30, 0x50, 0x32, 0x56, 0x58, 0x58, 0x5A, 0x5A,  /* $C0: green */
	0x30, 0x50, 0x32, 0x56, 0x58, 0x3A, 0x3C, 0x3C,  /* $D0: yellow green */
	0x20, 0x22, 0x24, 0x24, 0x2A, 0x2A, 0x2A, 0x2C,  /* $E0: orange green */
	0x20, 0x22, 0x24, 0x24, 0x28, 0x2A, 0x2A, 0x2C,  /* $F0: light orange */
};

uint8_t screen_colour(int system, uint8_t ntsc)
{
	return system == SYSTEM_NTSC ? ntsc : ntsc_to_pal[ntsc >> 1];
}

/* ---- Title screen ------------------------------------------------------- */

/* THE GAME, 48 pixels: six columns of 8, top row first (two lines each). */
const uint8_t screen_the_game[TITLE_LINES / 2][6] = {
	{0xFA, 0x2F, 0x80, 0x79, 0xC8, 0xBE},  /* XXXXX.X...X.XXXXX........XXXX..XXX..X...X.XXXXX. */
	{0x22, 0x28, 0x00, 0x82, 0x2D, 0xA0},  /* ..X...X...X.X...........X.....X...X.XX.XX.X..... */
	{0x22, 0x28, 0x00, 0x82, 0x2A, 0xA0},  /* ..X...X...X.X...........X.....X...X.X.X.X.X..... */
	{0x23, 0xEF, 0x00, 0x9B, 0xEA, 0xBC},  /* ..X...XXXXX.XXXX........X..XX.XXXXX.X.X.X.XXXX.. */
	{0x22, 0x28, 0x00, 0x8A, 0x28, 0xA0},  /* ..X...X...X.X...........X...X.X...X.X...X.X..... */
	{0x22, 0x28, 0x00, 0x8A, 0x28, 0xA0},  /* ..X...X...X.X...........X...X.X...X.X...X.X..... */
	{0x22, 0x2F, 0x80, 0x7A, 0x28, 0xBE},  /* ..X...X...X.XXXXX........XXXX.X...X.X...X.XXXXX. */
};

/* The menu's font: 5 pixels wide (bits 7-3), 7 rows, only the letters the menu uses. */
static const struct { char letter; uint8_t rows[TEXT_LINES]; } font[] = {
	{'A', {0x70, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88}},
	{'C', {0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70}},
	{'E', {0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8}},
	{'H', {0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88}},
	{'I', {0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70}},
	{'L', {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8}},
	{'M', {0x88, 0xD8, 0xA8, 0xA8, 0x88, 0x88, 0x88}},
	{'N', {0x88, 0xC8, 0xA8, 0xA8, 0x98, 0x88, 0x88}},
	{'O', {0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70}},
	{'P', {0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80}},
	{'R', {0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x88}},
	{'S', {0x78, 0x80, 0x80, 0x70, 0x08, 0x08, 0xF0}},
	{'T', {0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20}},
	{'V', {0x88, 0x88, 0x88, 0x88, 0x88, 0x50, 0x20}},
	{'X', {0x88, 0x88, 0x50, 0x20, 0x50, 0x88, 0x88}},
	{'Y', {0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x20}},
	{'0', {0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70}},
	{'6', {0x30, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70}},
};

static const uint8_t *glyph(char letter)
{
	for (unsigned i = 0; i < sizeof font / sizeof font[0]; i++)
		if (font[i].letter == letter) return font[i].rows;
	return 0;                   /* a space, or a letter the font lacks */
}

/* A text centred in 48 pixels: 6 pixels a letter (5 and a space), 8 letters at most. */
static void write_text(uint8_t text[TEXT_LINES][6], const char *string)
{
	int length = 0;
	while (string[length]) length++;
	int left = (48 - (length * 6 - 1)) / 2;
	for (int line = 0; line < TEXT_LINES; line++) {
		uint64_t pixels = 0;    /* bit 47: the leftmost pixel */
		for (int i = 0; i < length; i++) {
			const uint8_t *rows = glyph(string[i]);
			if (rows) pixels |= ((uint64_t)rows[line] << 40) >> (left + 6 * i);
		}
		for (int column = 0; column < 6; column++) text[line][column] = (uint8_t)(pixels >> (40 - 8 * column));
	}
}

static const char *const row_names[] = {"PLAY", "SYSTEM", "HMOVE", "EXIT"};
static const char *const system_names[SYSTEMS] = {"NTSC", "PAL60", "PAL"};
static const char *const hmove_names[2] = {"NORMAL", "SHORT"};

#define CART_SIDES       0x06   /* the cartridge body's grey in title.vcsgfx */
#define TITLE_COLOUR     0x9C   /* THE GAME: light blue */
#define CHOSEN_COLOUR    0x1C   /* the chosen row: yellow; its value: white; the rest: grey */
#define VALUE_COLOUR     0x0E
#define ROW_COLOUR       0x06

/* The menu as texts: the rows, and under the chosen one its value (SYSTEM and HMOVE). */
void screen_title(TitleScreen *title, const Menu *menu)
{
	int system = menu->system;
	for (int row = 0; row < CART_ROWS; row++) title->cart_colour[row] = screen_colour(system, cart_colupf[row]);
	title->sides_colour = screen_colour(system, CART_SIDES);
	title->title_colour = screen_colour(system, TITLE_COLOUR);
	title->texts = 0;
	for (int i = 0; i < menu->count; i++) {
		int row = menu->rows[i], chosen = i == menu->row;
		write_text(title->text[title->texts], row_names[row]);
		title->text_colour[title->texts++] = screen_colour(system, chosen ? CHOSEN_COLOUR : ROW_COLOUR);
		const char *value = !chosen ? 0 : row == MENU_SYSTEM ? system_names[system] : row == MENU_HMOVE ? hmove_names[menu->short_hmove] : 0;
		if (value) {
			write_text(title->text[title->texts], value);
			title->text_colour[title->texts++] = screen_colour(system, VALUE_COLOUR);
		}
	}
}

/* ---- The game ---------------------------------------------------------- */

/* The digits 0-9 for PF1, five rows each, top first, in both halves of the byte. */
static const uint8_t digits[10][SCORE_ROWS] = {
	{0xEE, 0xAA, 0xAA, 0xAA, 0xEE}, {0x22, 0x22, 0x22, 0x22, 0x22}, {0xEE, 0x22, 0xEE, 0x88, 0xEE},
	{0xEE, 0x22, 0x66, 0x22, 0xEE}, {0xAA, 0xAA, 0xEE, 0x22, 0x22}, {0xEE, 0x88, 0xEE, 0x22, 0xEE},
	{0xEE, 0x88, 0xEE, 0xAA, 0xEE}, {0xEE, 0x22, 0x22, 0x22, 0x22}, {0xEE, 0xAA, 0xEE, 0xAA, 0xEE},
	{0xEE, 0xAA, 0xEE, 0x22, 0xEE},
};

#define SCORE_COLOUR     0x0E   /* white */
#define LIVES_COLOUR     0x1C   /* yellow */
#define OTHER_ENEMY_HUE  0x40   /* the second enemy: another hue */

/* The frame an animation shows after a number of video frames (it loops). */
static int animation_frame(const vcsgfx_animation *animation, unsigned frames)
{
	unsigned length = 0;
	for (int i = 0; i < animation->count; i++) length += animation->steps[i].ticks;
	if (!length) return 0;
	frames %= length;
	for (int i = 0; i < animation->count; i++) {
		if (frames < animation->steps[i].ticks) return animation->steps[i].frame;
		frames -= animation->steps[i].ticks;
	}
	return 0;
}

/* An enemy into the pairs' tables. */
static void draw_enemy(GameScreen *screen, const Enemy *enemy, int which, int frame, int system)
{
	for (int row = 0; row < ENEMY_PAIRS; row++) {
		int pair = enemy->y + row;
		if (pair < 0 || pair >= ROOM_PAIRS) continue;
		screen->grp1[pair] = enemy_P1_gfx[frame][row];
		screen->colup1[pair] = screen_colour(system, (uint8_t)(enemy_P1_colour[frame][row] + (which ? OTHER_ENEMY_HUE : 0)));
	}
}

void screen_game(GameScreen *screen, const Game *game, int system)
{
	static vcsgfx_player hero;  /* the hero's animation */
	vcsgfx_play(&hero, game->walking ? &hero_walk : &hero_idle);
	int hero_frame = vcsgfx_animate(&hero), enemy_frame = animation_frame(&enemy_crawl, game->frame);

	/* the score's two digits on the left, the lives on the right */
	for (int row = 0; row < SCORE_ROWS; row++) {
		screen->score_pf1[row] = (uint8_t)((digits[game->score / 10][row] & 0xF0) | (digits[game->score % 10][row] & 0x0F));
		screen->lives_pf1[row] = (uint8_t)(digits[game->lives][row] & 0x0F);
	}
	screen->score_colour = screen_colour(system, SCORE_COLOUR);
	screen->lives_colour = screen_colour(system, LIVES_COLOUR);
	screen->wall_colour = screen_colour(system, ROOM_COLUPF);

	for (int pair = 0; pair < ROOM_PAIRS; pair++)
		screen->grp0[pair] = screen->colup0[pair] = screen->grp1[pair] = screen->colup1[pair] = screen->enam0[pair] = 0;
	/* the hero, mirrored when it faces left */
	screen->hero_x = game->hero_x;
	screen->refp0 = game->facing_left ? 0x08 : 0x00;
	for (int row = 0; row < HERO_PAIRS; row++) {
		screen->grp0[game->hero_y + row] = hero_P0_gfx[hero_frame][row];
		screen->colup0[game->hero_y + row] = screen_colour(system, hero_P0_colour[hero_frame][row]);
	}
	/* the shot (missile 0 stays off when there is none) */
	screen->shot_x = game->shot_on ? game->shot_x : 8;
	if (game->shot_on) screen->enam0[game->shot_y] = 0x02;

	/* the enemies: the upper one from the top of the frame; the lower one after player 1
	 * is moved, if there is a pair between them; if not, one on each frame */
	const Enemy *upper = 0, *lower = 0;
	int upper_which = 0, lower_which = 0;
	for (int i = 0; i < ENEMIES; i++) {
		const Enemy *enemy = &game->enemy[i];
		if (!enemy->alive) continue;
		if (!upper || enemy->y < upper->y) lower = upper, lower_which = upper_which, upper = enemy, upper_which = i;
		else lower = enemy, lower_which = i;
	}
	screen->band_pair = -1;
	screen->enemy_x = upper ? upper->x : 0;
	if (upper && lower && lower->y < upper->y + ENEMY_PAIRS + 1) {
		if (game->frame & 1) upper = lower, upper_which = lower_which;   /* they share lines: flicker */
		lower = 0;
		screen->enemy_x = upper->x;
	}
	if (upper) draw_enemy(screen, upper, upper_which, enemy_frame, system);
	if (lower) {
		draw_enemy(screen, lower, lower_which, enemy_frame, system);
		screen->band_pair = upper->y + ENEMY_PAIRS;
		screen->band_x = lower->x;
	}
}
