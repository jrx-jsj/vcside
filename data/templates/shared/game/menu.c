/*
 * {{NAME}} - the title screen's menu (see menu.h).
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 */
#include "menu.h"

void menu_init(Menu *menu, int system, int has_hmove, int short_hmove, int has_exit)
{
	menu->count = 0;
	menu->rows[menu->count++] = MENU_PLAY;
	menu->rows[menu->count++] = MENU_SYSTEM;
	if (has_hmove) menu->rows[menu->count++] = MENU_HMOVE;
	if (has_exit) menu->rows[menu->count++] = MENU_EXIT;
	menu->row = 0;              /* PLAY */
	menu->system = system;
	menu->short_hmove = short_hmove;
	menu->last_swcha = 0xFF;    /* as if released: a stick held at power on does nothing */
}

/* A step of a row's value: +1 (right, or the button) or -1 (left). */
static void change(Menu *menu, int step)
{
	switch (menu->rows[menu->row]) {
	case MENU_SYSTEM: menu->system = (menu->system + step + SYSTEMS) % SYSTEMS; break;
	case MENU_HMOVE: menu->short_hmove = !menu->short_hmove; break;
	}
}

int menu_frame(Menu *menu, uint8_t swcha, int fire)
{
	uint8_t pressed = (uint8_t)(~swcha & menu->last_swcha);   /* down now, up before (1 bits) */
	menu->last_swcha = swcha;
	if (pressed & 0x10) menu->row = (menu->row + menu->count - 1) % menu->count;   /* up */
	if (pressed & 0x20) menu->row = (menu->row + 1) % menu->count;                 /* down */
	if (pressed & 0x80) change(menu, 1);                                           /* right */
	if (pressed & 0x40) change(menu, -1);                                          /* left */
	if (fire) {
		int row = menu->rows[menu->row];
		if (row == MENU_PLAY || row == MENU_EXIT) return row;
		change(menu, 1);
	}
	return -1;
}
