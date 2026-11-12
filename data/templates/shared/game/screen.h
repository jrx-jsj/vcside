/*
 * {{NAME}} - what the kernels draw, worked out in the overblank: the title screen's texts
 * and colours, and the game's room line pair by line pair. Plain C (no bus calls): the
 * kernel only copies these tables to the TIA, since between two bus calls there is no time
 * for anything else.
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 */
#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include "game.h"
#include "menu.h"
#include "title.gfx.h"

/* The colours are written for NTSC (the .vcsgfx files, the tables here); PAL60 and PAL
 * take the closest PAL colour to each one. */
uint8_t screen_colour(int system, uint8_t ntsc);

/* ---- The title screen: VCSIDE's cartridge (title.vcsgfx's cart: an asymmetric playfield
 * with a colour per row, and the two missiles as its label's grey sides), THE GAME and the
 * menu, both 48 pixels wide (players 0 and 1, three copies each). */
#define TEXT_LINES       7      /* the font's height: one line per row */
#define TITLE_LINES      14     /* THE GAME: 7 rows of two lines */
#define MENU_TEXTS       5      /* the rows, and the chosen row's value under it */
extern const uint8_t screen_the_game[TITLE_LINES / 2][6];

typedef struct {
	uint8_t cart_colour[CART_ROWS];         /* COLUPF of each row of the cartridge */
	uint8_t sides_colour, title_colour;     /* the label's sides (the missiles), THE GAME */
	int texts;                              /* the menu's texts, top first */
	uint8_t text[MENU_TEXTS][TEXT_LINES][6];  /* GRP0, GRP1, GRP0, GRP1, GRP0, GRP1 of each line */
	uint8_t text_colour[MENU_TEXTS];
} TitleScreen;

void screen_title(TitleScreen *title, const Menu *menu);

/* ---- The game: the score and the lives (playfield, score mode), then the room, mirrored.
 * Each line pair of the room is two lines: the first writes the playfield (on the rows'
 * first lines) and the shot; the second the players, which stay on into the next pair's
 * first line. Both enemies share player 1: when one is well above the other, the kernel
 * moves player 1 between them (band_pair); when they share lines, they take turns, one
 * per frame. */
#define SCORE_ROWS       5      /* the digits' rows, two lines each */

typedef struct {
	uint8_t score_pf1[SCORE_ROWS];          /* the score: the left half's PF1 */
	uint8_t lives_pf1[SCORE_ROWS];          /* the lives: the right half's PF1 */
	uint8_t score_colour, lives_colour, wall_colour;
	int hero_x, enemy_x, shot_x;            /* player 0, player 1 at the top, missile 0 */
	uint8_t refp0;
	uint8_t grp0[ROOM_PAIRS], colup0[ROOM_PAIRS];   /* the hero */
	uint8_t grp1[ROOM_PAIRS], colup1[ROOM_PAIRS];   /* the enemies */
	uint8_t enam0[ROOM_PAIRS];                      /* the shot */
	int band_pair;          /* -1, or the pair whose second line moves player 1 to band_x */
	int band_x;             /* (the lower enemy); the next pair's first line ends the move */
} GameScreen;

void screen_game(GameScreen *screen, const Game *game, int system);

#endif
