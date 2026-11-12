/*
 * {{NAME}} - the title screen's menu, in plain C (no bus calls): PLAY, SYSTEM, and where
 * the cartridge has them, HMOVE and EXIT. Up and down choose a row, left and right change
 * its value, the button acts on it. The game starts only from PLAY.
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 */
#ifndef MENU_H
#define MENU_H

#include <stdint.h>

/* The TV systems, in the menu's order. NTSC: 262 lines, NTSC colours; PAL60: 262 lines,
 * PAL colours; PAL: 312 lines (50 Hz), PAL colours. */
#define SYSTEM_NTSC    0
#define SYSTEM_PAL60   1
#define SYSTEM_PAL     2
#define SYSTEMS        3

#define MENU_PLAY      0
#define MENU_SYSTEM    1
#define MENU_HMOVE     2        /* NORMAL / SHORT: the late HMOVE of some consoles (screen.h) */
#define MENU_EXIT      3        /* back to the cartridge's menu */

typedef struct {
	int rows[4], count;         /* the rows shown (MENU_*), top first */
	int row;                    /* the chosen one, in rows[] */
	int system;                 /* SYSTEM_* */
	int short_hmove;            /* HMOVE: SHORT */
	uint8_t last_swcha;         /* the joystick on the last frame */
} Menu;

/* has_hmove and has_exit: whether those rows are shown. */
void menu_init(Menu *menu, int system, int has_hmove, int short_hmove, int has_exit);
/* One frame of the menu: MENU_PLAY or MENU_EXIT when the button is pressed on them, else -1. */
int menu_frame(Menu *menu, uint8_t swcha, int fire);

#endif
