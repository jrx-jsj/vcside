/*
 * {{NAME}} - the two screens of VCSIDE The Game (2600 + 7800), which main.c chooses between,
 * and what the 7800's uses of the music player (music.h defines it, so only tia_game.c
 * includes it). CC0: use it as you like.
 */
#ifndef SCREENS_H
#define SCREENS_H

#include <stdint.h>

int tia_game_run(uint32_t *args);    /* tia_game.c: the 2600 (the TIA) */
int maria_game_run(uint32_t *args);  /* maria_game.c: the 7800 (MARIA) */

/* The music player (music.h, in tia_game.c) */
void music_init(void);
void music_frame(int lines);
extern uint8_t music_level[], music_nibbles[], music_tia[];
void game_sound_events(int events);  /* GAME_SHOT, GAME_HIT, GAME_LOSE as sound effects */

#endif
