/*
 * {{NAME}} - the two screens: the 2600's (tia_screen.c) and the 7800's (maria_screen.c).
 * elf_main (main.c) runs one of them; neither returns, except the 2600's when it goes back to
 * a multicart's menu. CC0.
 */
#ifndef SCREENS_H
#define SCREENS_H

#include <stdint.h>

int tia_screen_run(uint32_t *args);
int maria_screen_run(uint32_t *args);

#endif
