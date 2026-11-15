/*
 * {{NAME}} - made with VCSIDE: one ELF game for the Atari 2600 and the Atari 7800, on the
 * Chameleon Cart.
 *
 * The game (game.c: the room, the hero, the music) is plain C and the same on both consoles;
 * only the screen changes. On a 2600 the TIA draws it: the ARM feeds the 6507 each instruction
 * of the visible lines (tia_screen.c). On a 7800 MARIA draws it: the 6502 waits in a loop in
 * the console's RAM while the ARM serves MARIA the bytes of each line as it reads them
 * (maria_screen.c). Both show the same 160 x 192 room and play the same TIA music.
 *
 * Run the 2600 side with F5 (Stella or Gopher2600) and the 7800 side with Build > Run on the
 * Atari 7800 (Test7800). Build: F7. This template is CC0: use it as you like.
 */
#include "vcsLib.h"
#include "game.h"
#include "screens.h"

/* Firmwares that run ELF games on a 7800 look for these sections: with .elf7800 the game
 * starts in MARIA mode with args[MP_SYSTEM_TYPE] = ST_NTSC_7800 or ST_PAL_7800; with only
 * .elf2600 (or neither) a 7800 is locked in 2600 mode. One byte each. */
__attribute__((section(".elf2600"), used)) static const uint8_t runs_on_2600 = 1;
__attribute__((section(".elf7800"), used)) static const uint8_t runs_on_7800 = 1;

int elf_main(uint32_t *args)
{
	game_init();
	unsigned system = args[MP_SYSTEM_TYPE];
	if (system == ST_NTSC_7800 || system == ST_PAL_7800) return maria_screen_run(args);
	return tia_screen_run(args);
}
