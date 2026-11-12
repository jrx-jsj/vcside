# {{NAME}}: notes for the assistant

VCSIDE The Game in C on an ACE cartridge (PlusCart, UnoCart; Gopher2600): a room with a hero, two enemies, a shot, score and lives, and a title screen with a menu (PLAY, SYSTEM: NTSC / PAL60 / PAL, EXIT). The ARM (Cortex-M4) feeds the 6507 every instruction of the visible lines through VCSIDE's bus library (`include/ace.h`, `src/ace.c`); during the overblank the 6507 runs VSYNC and the blank lines from its own RAM and the ARM runs the game.

## Kernel rules
- Between two `ace_` calls the 6507 is already waiting: the kernels only copy values worked out in the overblank (`screen.c`, `place`, `band_timing`); no divisions or loops between calls. The comments give the cycle where each write ends.
- `ace_jmp()` once a line, except where a write ends after cycle 70 (the next `WSYNC` would fall on the next line).
- Every visible line writes its sample to `AUDV0` (`*sample++`); the overblank's 31 bytes go to `$E0`-`$FE` on the first lines (`overblank_samples`). The kernels must use exactly `lines` lines: 199 (NTSC, PAL60) or 249 (PAL), chosen at run time by the menu (it starts at the project's TV standard: ACE gives no system).
- The room: two lines a pair (`ROOM_PAIRS`); the first writes PF0-PF2 and the shot, the second the players. Both enemies share player 1: when one is a pair or more below the other, `band_pair` moves player 1 with `RESP1` and an HMOVE at cycle 73 (no black comb); otherwise they flicker.
- Colours are NTSC values; `screen_colour` turns them into PAL ones for PAL60 and PAL.
- The start code (`src/start.S`) must follow the 40-byte header (`ace.ld`; the Makefile checks it).

## Where things live
- `main()` in `src/main.c`: the loop, the kernels and the sound. EXIT: `leave()`, and back to the cartridge's menu.
- `src/game.c`: the rules (`game_frame`: hero, shot, enemies, collisions with the room's own tables, score, lives). `src/menu.c`: the menu. `src/screen.c`: what the kernels draw. These three have no bus calls (the ELF template shares them).
- Sound: `music_effect(MUSIC_EFFECT_SHOT / _HIT / _LOSE)` from `game_frame`'s events; "hit" plays on the TIA's channel 1 (`music_tia`).
- RESET in the game: back to the title.

## Build
- F7 runs `make`: `build/{{NAME}}.ace` and `build/ACE_debugging.elf` (for Gopher2600's debugger). Runs in Gopher2600 only (Stella has no ACE).

## Do not touch
- `include/ace.h`, `src/ace.c`, `src/start.S`, `src/memory.c`, `ace.ld`.
- `*.gfx.h` and `theme.music.h`: written by VCSIDE when `hero.vcsgfx`, `enemy.vcsgfx`, `room.vcsgfx`, `title.vcsgfx` and `theme.vcsmusic` are saved.
