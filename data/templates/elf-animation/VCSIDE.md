# {{NAME}}: notes for the assistant

An ELF cartridge game in C from VCSIDE's animated sprite example: a hero that walks in four directions. The cartridge's ARM feeds the 6507 every instruction of the visible lines through vcsLib (`include/vcsLib.h`); during the overblank the 6507 runs VSYNC and the blank lines by itself and the ARM runs the game logic.

## Kernel rules
- Between two vcsLib calls the 6507 is already waiting: work is done only right after a `vcsSta3(WSYNC)` or in the overblank.
- `kernel()` and the functions it calls are in RAM (`RAM_FUNC`, `RAM_K` for `position_p0`): code in flash is too slow.
- `vcsJmp3()` once a line, so the 6507's program counter stays in its 4 KB.
- Every visible line writes its sample to `AUDV0`; lines 2 to 32 also write the overblank's samples to `$E0`-`$FE`.
- Each sprite row takes two lines (`LINES_PER_ROW`). Visible lines: 199, or 249 on PAL.
- Calls to the loader are long (`#pragma long_calls` in `vcsLib.h`); only loader functions may stay undefined (the Makefile checks both).

## Where things live
- `elf_main(args)` in `src/main.c`: `args[MP_SYSTEM_TYPE]` (PAL), `args[MP_FEATURE_FLAGS]` (`FF_MULTI_CART`: RESET leaves; `FF_CHAMELEON_CART`: `vcsEndOverblank()` first).
- Joystick and button: read by `kernel()` (`swcha`, `swchb`, `inpt4`), used in `elf_main`'s loop.
- The hero: `position_p0`, `kernel()`; its animation: `vcsgfx_play(&hero, ...)` with `hero_idle` / `hero_walk` from `hero.gfx.h`.
- Music: `music_init`, `music_frame`, `music_effect(MUSIC_EFFECT_FIRE / _BUMP)`.

## Build
- F7 runs `make`: `build/{{NAME}}.bin`. `make ace` and `make chameleon` as in the Makefile.

## Do not touch
- `include/vcsLib.h`, `src/memcpy_long.S`, `ace/`, `chameleon/`, the Makefile's checks.
- `src/hero.gfx.h`, `src/music.h`: written by VCSIDE when `hero.vcsgfx` / `theme.vcsmusic` are saved. Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `music_play(MUSIC_SONG_<NAME>)` starts another song, `music_stop()` stops it.
