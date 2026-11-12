/*
 * {{NAME}} - the rules of VCSIDE The Game, in plain C: no bus calls, so the same file
 * works on every ARM cartridge (ELF, ACE). The kernel draws what screen.c makes of it.
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * The room is room.vcsgfx: ROOM_ROWS rows of ROOM_LINES lines. Places are in pixels
 * across (0-159) and in line pairs down from the room's top (0 to ROOM_PAIRS - 1): the
 * sprites' rows are two lines tall. The hero is player 0, the enemies share player 1 and
 * the shot is missile 0. Walls and blocks are found in the room's own tables, and what
 * touches what is worked out here too, so the TIA's collision registers are not needed.
 */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include "hero.gfx.h"
#include "enemy.gfx.h"
#include "room.gfx.h"

#define ROOM_PAIRS     (ROOM_ROWS * ROOM_LINES / 2)
#define SPRITE_WIDTH   8
#define HERO_PAIRS     HERO_P0_HEIGHT
#define ENEMY_PAIRS    ENEMY_P1_HEIGHT
#define ENEMIES        2
#define LIVES          3
#define SHOT_WIDTH     4        /* missile 0 at four clocks (NUSIZ0 = 0x20) */
#define SHOT_SPEED     3

/* What happened on a frame, for the sound effects: game_frame's result. */
#define GAME_SHOT      1        /* the hero fired */
#define GAME_HIT       2        /* the shot hit an enemy */
#define GAME_LOSE      4        /* an enemy touched the hero: a life less */
#define GAME_OVER      8        /* that was the last life */

typedef struct {
	int x, y;                   /* top left corner */
	int dx, dy;                 /* +1 or -1: one pixel and one pair on its frames */
	int alive;
} Enemy;

typedef struct {
	int hero_x, hero_y;
	int facing_left, walking;   /* REFP0, and the animation (idle or walk) */
	int shot_x, shot_y, shot_dx, shot_on;
	Enemy enemy[ENEMIES];
	int score, lives;
	unsigned frame;
} Game;

/* The joystick as SWCHA gives it (0 bits: pressed) and the button just pressed. */
void game_start(Game *game);
int game_frame(Game *game, uint8_t swcha, int fire);

#endif
