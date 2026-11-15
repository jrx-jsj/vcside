/*
 * {{NAME}} - the game, shared by both consoles (see game.h). The hero's frames, colours and
 * animations are drawn in hero.vcsgfx and the song is theme.vcsmusic, a TIA song: open them
 * in VCSIDE; saving them writes hero.gfx.h and music.h, which only this file includes.
 *
 * The music is the TIA's on both consoles: the 7800 has the same TIA, clocked the same way,
 * and a Chameleon Cart adds no POKEY. music_frame gives AUDC, AUDF and AUDV for each channel
 * once a frame; each screen writes them to the TIA. "fire" plays on the button and "bump"
 * when the hero reaches a wall, both on channel 1.
 */
#include "game.h"
#include "hero.gfx.h"
#include "music.h"

_Static_assert(HERO_P0_HEIGHT <= HERO_MAX_ROWS, "hero.vcsgfx: frames taller than HERO_MAX_ROWS");

Game game;
static vcsgfx_player hero;
static uint8_t last_fire, last_pause;

#define LEFT_EDGE   WALL_PIXELS
#define RIGHT_EDGE  (ROOM_WIDTH - WALL_PIXELS - HERO_WIDTH)
#define TOP_EDGE    WALL_LINES
#define BOTTOM_EDGE (ROOM_LINES - WALL_LINES - HERO_P0_HEIGHT * LINES_PER_ROW)

static int at_edge(void)
{
	return game.hero_x == LEFT_EDGE || game.hero_x == RIGHT_EDGE || game.hero_y == TOP_EDGE || game.hero_y == BOTTOM_EDGE;
}

static void show_frame(int frame)
{
	game.hero_gfx = hero_P0_gfx[frame];
	game.hero_colour = hero_P0_colour[frame];
}

void game_init(void)
{
	game.hero_x = (ROOM_WIDTH - HERO_WIDTH) / 2;
	game.hero_y = (ROOM_LINES - HERO_P0_HEIGHT * LINES_PER_ROW) / 2;
	game.hero_rows = HERO_P0_HEIGHT;
	show_frame(0);
	music_init();
}

void game_frame(const GameInput *input)
{
	/* PAUSE (7800) stops everything on each press, silence included */
	if (input->pause && !last_pause) game.paused = !game.paused;
	last_pause = input->pause;
	if (game.paused) {
		for (int i = 0; i < 6; i++) game.audio[i] = 0;
		return;
	}
	int moving = 0, was_at_edge = at_edge();
	if (input->right && game.hero_x < RIGHT_EDGE) game.hero_x++, game.facing_left = 0, moving = 1;
	if (input->left && game.hero_x > LEFT_EDGE) game.hero_x--, game.facing_left = 1, moving = 1;
	if (input->up && game.hero_y > TOP_EDGE) game.hero_y--, moving = 1;
	if (input->down && game.hero_y < BOTTOM_EDGE) game.hero_y++, moving = 1;
	if (moving && !was_at_edge && at_edge()) music_effect(MUSIC_EFFECT_BUMP);  /* it has just reached a wall */
	if (input->fire && !last_fire) music_effect(MUSIC_EFFECT_FIRE);
	last_fire = input->fire;
	vcsgfx_play(&hero, moving ? &hero_walk : &hero_idle);
	show_frame(vcsgfx_animate(&hero));
	music_frame();
	for (int channel = 0; channel < 2; channel++)
		for (int i = 0; i < 3; i++) game.audio[channel * 3 + i] = channel < MUSIC_CHANNELS ? music_audio[channel][i] : 0;
}

int game_colours(uint8_t *colours, int max)
{
	int count = 0;
	uint8_t wanted[2 + HERO_FRAMES * HERO_P0_HEIGHT];
	int n = 0;
	wanted[n++] = FLOOR_COLOUR;
	wanted[n++] = WALL_COLOUR;
	for (int frame = 0; frame < HERO_FRAMES; frame++)
		for (int row = 0; row < HERO_P0_HEIGHT; row++) wanted[n++] = hero_P0_colour[frame][row];
	for (int i = 0; i < n; i++) {
		int known = 0;
		for (int j = 0; j < count; j++) known |= colours[j] == wanted[i];
		if (!known && count < max) colours[count++] = wanted[i];
	}
	return count;
}
