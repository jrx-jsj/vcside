/*
 * {{NAME}} - the Atari 7800's pictures (art7800.c): the walls' wood, the floor's tiles, the
 * hero, the enemies and the title screen's cartridge, as text. CC0: use it as you like.
 */
#ifndef ART7800_H
#define ART7800_H

#include <stdint.h>
#include "hero.gfx.h"
#include "enemy.gfx.h"

#define ART_COLOURS      12      /* 160B: four palettes of three colours, and black */
#define ART_WOOD_WIDTH   32
#define ART_WOOD_LINES   8
#define ART_FLOOR_WIDTH  32
#define ART_FLOOR_LINES  32
#define ART_SPRITE_WIDTH 8
#define ART_HERO_LINES   (2 * HERO_P0_HEIGHT)    /* the 2600 hero's rows, a line each */
#define ART_ENEMY_LINES  (2 * ENEMY_P1_HEIGHT)
#define ART_CART_WIDTH   80
#define ART_CART_LINES   88

extern const char art_game_letters[ART_COLOURS + 1];
extern const uint8_t art_game_colours[ART_COLOURS];
extern const char *const art_wood[ART_WOOD_LINES];
extern const char *const art_floor[ART_FLOOR_LINES];
extern const char *const art_hero[HERO_FRAMES][ART_HERO_LINES];
extern const char *const art_enemy[ENEMY_FRAMES][ART_ENEMY_LINES];

extern const char art_title_letters[ART_COLOURS + 1];
extern const uint8_t art_title_colours[ART_COLOURS];
extern const char *const art_cart[ART_CART_LINES];

#endif
