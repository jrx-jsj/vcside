/*
 * {{NAME}} - the rules of VCSIDE The Game (see game.h). Runs in the overblank, while the
 * 6507 is busy with VSYNC and the blank lines: nothing here talks to the bus.
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 */
#include "game.h"

#define HERO_START_X   76
#define HERO_START_Y   (ROOM_PAIRS - 8 - HERO_PAIRS)   /* near the bottom, under the blocks */

/* Where each enemy starts a wave: at different heights (each one gets player 1 for its own
 * band of the screen), both walking right and down. */
static const int wave_x[ENEMIES] = {40, 104}, wave_y[ENEMIES] = {6, 30}, wave_dx[ENEMIES] = {1, 1};

/* Is the room's playfield on at column (0-39) and row? The tables are PF0, PF1 and PF2 of
 * the left half (PF0's bits 4-7, PF1's 7-0 and PF2's 0-7, left to right); the right half
 * mirrors them (ROOM_CTRLPF = 1) or repeats them. */
static int room_wall(int column, int row)
{
    if (column >= 20) column = (ROOM_CTRLPF & 1) ? 39 - column : column - 20;
    if (column < 4) return (room_pf0[row] >> (4 + column)) & 1;
    if (column < 12) return (room_pf1[row] >> (11 - column)) & 1;
    return (room_pf2[row] >> (column - 12)) & 1;
}

/* Does a box of width pixels, from line top to line bottom of the room, touch a wall? The
 * screen's edges count as walls. */
static int touches_wall(int x, int width, int top, int bottom)
{
    if (x < 0 || x + width > 160 || top < 0 || bottom >= ROOM_ROWS * ROOM_LINES) return 1;
    for (int row = top / ROOM_LINES; row <= bottom / ROOM_LINES; row++)
        for (int column = x / 4; column <= (x + width - 1) / 4; column++)
            if (room_wall(column, row)) return 1;
    return 0;
}

/* A sprite at pair y is drawn on the room's lines 2y + 1 to 2y + 2 x pairs (the kernel
 * writes it on the second line of each pair). */
static int sprite_touches_wall(int x, int y, int pairs)
{
    return touches_wall(x, SPRITE_WIDTH, 2 * y + 1, 2 * (y + pairs));
}

/* Do two boxes overlap? (left, right, top, bottom, all inclusive) */
static int overlap(int left1, int right1, int top1, int bottom1, int left2, int right2, int top2, int bottom2)
{
    return left1 <= right2 && left2 <= right1 && top1 <= bottom2 && top2 <= bottom1;
}

static void place_hero(Game *game)
{
    game->hero_x = HERO_START_X;
    game->hero_y = HERO_START_Y;
    game->shot_on = 0;
}

static void new_wave(Game *game)
{
    for (int i = 0; i < ENEMIES; i++) {
        Enemy *enemy = &game->enemy[i];
        enemy->x = wave_x[i];
        enemy->y = wave_y[i];
        enemy->dx = wave_dx[i];
        enemy->dy = 1;          /* down */
        enemy->alive = 1;
    }
}

void game_start(Game *game)
{
    game->score = 0;
    game->lives = LIVES;
    game->facing_left = 0;
    game->walking = 0;
    place_hero(game);
    new_wave(game);
}

/* The joystick moves the hero one pixel across or one pair up or down a frame; a wall or a
 * block stops it on that axis only, so it slides along them. */
static void move_hero(Game *game, uint8_t swcha)
{
    int dx = 0, dy = 0;
    if (!(swcha & 0x80)) dx = 1, game->facing_left = 0;     /* right (bits: 0 pressed) */
    if (!(swcha & 0x40)) dx = -1, game->facing_left = 1;    /* left */
    if (!(swcha & 0x10)) dy = -1;                           /* up */
    if (!(swcha & 0x20)) dy = 1;                            /* down */
    game->walking = dx || dy;
    if (dx && !sprite_touches_wall(game->hero_x + dx, game->hero_y, HERO_PAIRS)) game->hero_x += dx;
    if (dy && !sprite_touches_wall(game->hero_x, game->hero_y + dy, HERO_PAIRS)) game->hero_y += dy;
}

/* The button fires one shot at a time, from the side the hero faces, at its middle; it flies
 * until it reaches a wall. */
static int shoot(Game *game, int fire)
{
    int events = 0;
    if (fire && !game->shot_on) {
        game->shot_on = 1;
        game->shot_y = game->hero_y + HERO_PAIRS / 2;
        game->shot_dx = game->facing_left ? -SHOT_SPEED : SHOT_SPEED;
        game->shot_x = game->facing_left ? game->hero_x - SHOT_WIDTH : game->hero_x + SPRITE_WIDTH;
        events |= GAME_SHOT;
    } else if (game->shot_on) {
        game->shot_x += game->shot_dx;
    }
    /* the missile is drawn on both lines of its pair */
    if (game->shot_on && touches_wall(game->shot_x, SHOT_WIDTH, 2 * game->shot_y, 2 * game->shot_y + 1)) game->shot_on = 0;
    return events;
}

/* Each enemy walks diagonally on every other frame (they take turns) and turns round on the
 * axis that meets a wall. */
static void move_enemies(Game *game)
{
    Enemy *enemy = &game->enemy[game->frame & 1];
    if (!enemy->alive) return;
    if (sprite_touches_wall(enemy->x + enemy->dx, enemy->y, ENEMY_PAIRS)) enemy->dx = -enemy->dx;
    else enemy->x += enemy->dx;
    if (sprite_touches_wall(enemy->x, enemy->y + enemy->dy, ENEMY_PAIRS)) enemy->dy = -enemy->dy;
    else enemy->y += enemy->dy;
}

/* The shot against the enemies, and the enemies against the hero (a pixel in from each side:
 * the corners of the sprites are mostly empty). */
static int collisions(Game *game)
{
    int events = 0, alive = 0;
    for (int i = 0; i < ENEMIES; i++) {
        Enemy *enemy = &game->enemy[i];
        if (!enemy->alive) continue;
        if (game->shot_on && overlap(game->shot_x, game->shot_x + SHOT_WIDTH - 1, game->shot_y, game->shot_y,
                                     enemy->x, enemy->x + SPRITE_WIDTH - 1, enemy->y, enemy->y + ENEMY_PAIRS - 1)) {
            enemy->alive = 0;
            game->shot_on = 0;
            if (game->score < 99) game->score++;
            events |= GAME_HIT;
            continue;
        }
        if (overlap(game->hero_x + 1, game->hero_x + SPRITE_WIDTH - 2, game->hero_y, game->hero_y + HERO_PAIRS - 1,
                    enemy->x + 1, enemy->x + SPRITE_WIDTH - 2, enemy->y + 1, enemy->y + ENEMY_PAIRS - 2)) {
            events |= GAME_LOSE;
            break;
        }
        alive++;
    }
    if (events & GAME_LOSE) {
        place_hero(game);
        new_wave(game);         /* the enemies back at the top, away from the hero */
        if (--game->lives == 0) events |= GAME_OVER;
    } else if (!alive) {
        new_wave(game);         /* both gone: two more */
    }
    return events;
}

int game_frame(Game *game, uint8_t swcha, int fire)
{
    game->frame++;
    move_hero(game, swcha);
    int events = shoot(game, fire);
    move_enemies(game);
    return events | collisions(game);
}
