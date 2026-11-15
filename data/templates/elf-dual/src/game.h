/*
 * {{NAME}} - the game itself, the same on the Atari 2600 and on the Atari 7800: the room, the
 * hero, its animation and the music. Plain C with no vcsLib calls: tia_screen.c draws it with
 * the TIA, maria_screen.c with MARIA. CC0: use it as you like.
 */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>

/* The room, in the 2600's pixels (160 wide) and lines: walls 8 lines thick at the top and
 * the bottom and 4 pixels at the sides (PF0's bit 4 on the 2600, mirrored). */
#define ROOM_WIDTH     160
#define ROOM_LINES     192
#define WALL_LINES     8
#define WALL_PIXELS    4
#define FLOOR_COLOUR   0xD2      /* NTSC colour bytes: the same values on both consoles */
#define WALL_COLOUR    0x0C

#define HERO_WIDTH     8
#define HERO_MAX_ROWS  32
#define LINES_PER_ROW  2         /* each row of the hero is two lines tall */

/* What the console's controls say this frame (1 = pressed). */
typedef struct {
	uint8_t up, down, left, right;
	uint8_t fire;                /* the 2600 joystick's button, or either 7800 button */
	uint8_t pause;               /* the 7800's PAUSE button (the 2600 has none) */
} GameInput;

typedef struct {
	int hero_x, hero_y;          /* top left pixel of the hero, inside the room */
	int facing_left;
	int hero_rows;               /* rows of the hero's frames (hero.vcsgfx) */
	const uint8_t *hero_gfx;     /* the frame to draw: a byte per row, top first, bit 7 = left */
	const uint8_t *hero_colour;  /* and its colour per row */
	uint8_t audio[6];            /* AUDC0, AUDF0, AUDV0, AUDC1, AUDF1, AUDV1 for this frame */
	uint8_t paused;
} Game;

extern Game game;

void game_init(void);
/* Once per frame: moves the hero, plays the music; then game says what to draw and play. */
void game_frame(const GameInput *input);
/* Every colour the game can show apart from black, without repeats: the floor, the walls and
 * the hero's (all frames). The 7800 keeps them in its palettes. Returns how many (up to max). */
int game_colours(uint8_t *colours, int max);

#endif
