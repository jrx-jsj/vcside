/*
 * {{NAME}} - VCSIDE The Game for the Atari 2600 and the Atari 7800, made with VCSIDE: one ELF
 * file that runs on a 2600 (PlusCart, UnoCart, Chameleon Cart, Stella, Gopher2600) and, on a
 * Chameleon Cart, on a 7800.
 *
 * A room with walls where the hero walks and shoots at two enemies: hitting one scores a
 * point, touching one costs a life, and after the last life the title screen comes back.
 *
 *   game.c        the rules (movement, walls, enemies, shot, score, lives): both consoles
 *   menu.c        the title screen's menu: both consoles
 *   screen.c      what the 2600's kernels draw (the 7800 takes its texts and score from it)
 *   tia_game.c    the 2600: the TIA's kernels, as in the 2600-only template
 *   maria_game.c  the 7800: MARIA, with wood on the walls, tiled floors, new sprites and a
 *                 new cartridge on the title screen (drawn in art7800.c)
 *
 * Graphics: hero.vcsgfx, enemy.vcsgfx, room.vcsgfx and title.vcsgfx (the 2600's; the room's
 * walls and the sprites' boxes and animations are the 7800's too), and art7800.c. Music:
 * theme.vcsmusic, sampled, with the effects shot, hit and lose, on both consoles.
 *
 * Run the 2600 side with F5 (Stella or Gopher2600) and the 7800 side with Build > Run on the
 * Atari 7800 (Test7800). Build: F7. This template is CC0: use it as you like.
 */
#include "vcsLib.h"
#include "screens.h"

/* Firmwares that run ELF games on a 7800 look for these sections: with .elf7800 the game
 * starts in MARIA mode with args[MP_SYSTEM_TYPE] = ST_NTSC_7800 or ST_PAL_7800; with only
 * .elf2600 (or neither) a 7800 is locked in 2600 mode. One byte each. */
__attribute__((section(".elf2600"), used)) static const uint8_t runs_on_2600 = 1;
__attribute__((section(".elf7800"), used)) static const uint8_t runs_on_7800 = 1;

int elf_main(uint32_t *args)
{
	unsigned system = args[MP_SYSTEM_TYPE];
	if (system == ST_NTSC_7800 || system == ST_PAL_7800) return maria_game_run(args);
	return tia_game_run(args);
}
