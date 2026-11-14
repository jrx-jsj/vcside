# {{NAME}}: notes for the assistant

An ELF cartridge game in C from VCSIDE's template: the cartridge's ARM feeds the 6507 every instruction of the visible lines through vcsLib (`include/vcsLib.h`); during the overblank the 6507 runs VSYNC and the blank lines by itself and the ARM runs the game logic.

## Kernel rules
- Between two vcsLib calls the 6507 is already waiting: work is done only right after a `vcsSta3(WSYNC)` or in the overblank (between `vcsStartOverblank()` and `vcsEndOverblank()`).
- `kernel()` and every function the 6507 waits on are `RAM_FUNC` (`.RamFunc`): code in flash is too slow.
- `vcsJmp3()` once a line, so the 6507's program counter stays in its 4 KB.
- Every visible line writes its sample to `AUDV0` (`music_level`); the first 31 lines also write the overblank's samples to `$E0`-`$FE` (`music_nibbles`).
- Visible lines: 262 - 63 = 199, or 312 - 63 = 249 on PAL.
- Calls to the loader are long (`vcsLib.h` between `#pragma long_calls` and `long_calls_off`); only `vcs*`, `memcpy`, `memset`, `randint`, `ColorLookup` and `ReverseByte` may stay undefined (the Makefile checks both).

## Where things live
- `elf_main(args)` in `src/main.c`: `args[MP_SYSTEM_TYPE]` (PAL gives 249 lines), `args[MP_FEATURE_FLAGS]` with masks: `FF_MULTI_CART` (RESET goes back to the menu: `leave()`, then return), `FF_CHAMELEON_CART` (already in an overblank: `vcsEndOverblank()` first).
- Joystick, switches, button: read by `kernel()` after the visible lines (`swcha`, `swchb`, `inpt4`), used in `elf_main`'s loop.
- Music: `music_init`, `music_frame(lines + MUSIC_OVERBLANK_LINES)` once a frame, `music_effect(MUSIC_EFFECT_FIRE / _BUMP)`; "bump" plays on the TIA's channel 1 from `music_tia`. A song with a TIA track (*TIA track* in the music editor) plays it there too, through the same `music_tia`, when no effect sounds there.

## Folders
- `src/`: programs; `resources/gfx/`, `resources/music/`: `.vcsgfx`, `.vcsmusic`; `generated/`: what VCSIDE writes from them; `build/`: the build's. Set in the `.vcsproj` (`*_dir`).

## Build
- F7 runs `make`: `build/{{NAME}}.bin`, the ELF ROM (Stella, Gopher2600, PlusCart, UnoCart).{{EXTRA_BUILDS}}

## Do not touch
- `include/vcsLib.h` (the loader's interface), `src/memcpy_long.S`{{GLUE}}, and the Makefile's checks.
- `generated/music/music.h` (and any `generated/gfx/*.gfx.h`): written by VCSIDE when the `.vcsmusic`/`.vcsgfx` files are saved. Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `music_play(MUSIC_SONG_<NAME>)` starts another song, `music_stop()` stops it.
