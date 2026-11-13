# {{NAME}}: notes for the assistant

An ACE cartridge game in C (PlusCart, UnoCart) from VCSIDE's animated sprite example: a hero that walks in four directions. The ARM feeds the 6507 every instruction of the visible lines through VCSIDE's bus library (`include/ace.h`, `src/ace.c`); during the overblank the 6507 runs VSYNC and the blank lines from its own RAM and the ARM runs the game logic.

## Kernel rules
- Between two `ace_` calls the 6507 is already waiting: work is done only right after `ace_wsync()` or in the overblank.
- `ace_jmp()` once a line. Every visible line writes its sample to `AUDV0`; lines 2 to 32 also write the overblank's samples to `$E0`-`$FE`.
- Each sprite row takes two lines. Visible lines: `lines`, from the project's TV standard (ACE passes no system to the game).
- The start code (`src/start.S`) must follow the 40-byte header (`ace.ld`; the Makefile checks it).

## Where things live
- `main()` in `src/main.c`: the game loop. Joystick, switches, button: read by `kernel()`. RESET: `leave()`, back to the menu.
- The hero: `position_p0`, `kernel()`; its animation: `vcsgfx_play(&hero, ...)` with `hero_idle` / `hero_walk` from `hero.gfx.h`.
- Music: `music_init`, `music_frame`, `music_effect(MUSIC_EFFECT_FIRE / _BUMP)`.

## Build
- F7 runs `make`: `build/{{NAME}}.ace` and `build/ACE_debugging.elf`. Runs in Gopher2600 only.

## Do not touch
- `include/ace.h`, `src/ace.c`, `src/start.S`, `src/memory.c`, `ace.ld`.
- `src/hero.gfx.h`, `src/music.h`: written by VCSIDE when `hero.vcsgfx` / `theme.vcsmusic` are saved. Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `music_play(MUSIC_SONG_<NAME>)` starts another song, `music_stop()` stops it.
