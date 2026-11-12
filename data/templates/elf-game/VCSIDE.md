# {{NAME}}: notes for the assistant

VCSIDE The Game in C on an ELF cartridge (PlusCart, UnoCart, Chameleon Cart, Stella, Gopher2600): a room with a hero, two enemies, a shot, score and lives, and a title screen with a menu (PLAY, SYSTEM: NTSC / PAL60 / PAL, HMOVE: NORMAL / SHORT, and EXIT when started from a cartridge menu). The cartridge's ARM feeds the 6507 every instruction of the visible lines through vcsLib (`include/vcsLib.h`); during the overblank the 6507 runs VSYNC and the blank lines by itself and the ARM runs the game.

## Kernel rules
- Between two vcsLib calls the 6507 is already waiting: the kernels only copy values worked out in the overblank (`screen.c`, `place`, `band_timing`); no divisions or loops between calls. The comments give the cycle where each write ends.
- `title_kernel`, `game_kernel` and what they call are in RAM (`RAM_FUNC`, `RAM_K`): code in flash is too slow on the Chameleon.
- `vcsJmp3()` once a line, except where a write ends after cycle 70 (the next `WSYNC` would fall on the next line).
- Every visible line writes its sample to `AUDV0` (`*sample++`); the overblank's 31 bytes go to `$E0`-`$FE` on the first lines (`overblank_samples`). The kernels must use exactly `lines` lines: 199 (NTSC, PAL60) or 249 (PAL), chosen at run time by the menu.
- The room: two lines a pair (`ROOM_PAIRS`); the first writes PF0-PF2 and the shot, the second the players. Both enemies share player 1: when one is a pair or more below the other, `band_pair` moves player 1 with `RESP1` and an HMOVE at cycle 73 (no black comb; HMOVE: SHORT adds 1 to `HMP1` for consoles whose late HMOVE moves one pixel less); otherwise they flicker.
- Colours are NTSC values; `screen_colour` turns them into PAL ones for PAL60 and PAL.

## Where things live
- `elf_main(args)` in `src/main.c`: reads `MP_SYSTEM_TYPE` (PAL and PAL60 only), `MP_CLOCK_HZ` (not used: ticks on the Chameleon) and `MP_FEATURE_FLAGS` with masks (`FF_MULTI_CART`: EXIT; `FF_CHAMELEON_CART`: `vcsEndOverblank()` first; `FF_REDUCED_HMOVE`: HMOVE starts at SHORT). Returns only from EXIT.
- `src/game.c`: the rules (`game_frame`: hero, shot, enemies, collisions with the room's own tables, score, lives). `src/menu.c`: the menu. `src/screen.c`: what the kernels draw. These three have no bus calls (the ACE template shares them).
- Sound: `music_effect(MUSIC_EFFECT_SHOT / _HIT / _LOSE)` from `game_frame`'s events; "hit" plays on the TIA's channel 1 (`music_tia`).
- RESET in the game: back to the title.

## Build
- F7 runs `make`: `build/{{NAME}}.bin`, the ELF ROM. `make ace`: the same game as `build/{{NAME}}.ace` (vcsLib on VCSIDE's ACE bus library in `ace/`). `make chameleon CHAMELEON_FW=...`: the Chameleon Cart image.

## Do not touch
- `include/vcsLib.h`, `src/memcpy_long.S`, `ace/` and `chameleon/`, and the Makefile's checks.
- `*.gfx.h` and `theme.music.h`: written by VCSIDE when `hero.vcsgfx`, `enemy.vcsgfx`, `room.vcsgfx`, `title.vcsgfx` and `theme.vcsmusic` are saved.
