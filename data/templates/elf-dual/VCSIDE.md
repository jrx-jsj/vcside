# {{NAME}}: notes for the assistant

An ELF cartridge game in C from VCSIDE's dual example: one `.elf` that runs on the Atari 2600 and, on a Chameleon Cart, on the Atari 7800. A hero walks around a walled room to a TIA song, the same on both consoles. `elf_main` (`src/main.c`) reads `args[MP_SYSTEM_TYPE]`: `ST_NTSC_7800` and `ST_PAL_7800` go to MARIA's screen, everything else to the TIA's.

## Where things live
- `src/game.c`, `src/game.h`: the game itself (room, hero, animation, music), plain C with no vcsLib calls. Both screens draw `game` and feed `game_frame` a `GameInput` once a frame. Only `game.c` includes `hero.gfx.h` and `music.h`.
- `src/tia_screen.c`: the 2600. The ARM feeds the 6507 every instruction of the visible lines; during the overblank the 6507 runs VSYNC, VBLANK and overscan by itself and the ARM runs the game.
- `src/maria_screen.c`: the 7800. The console's RAM holds a display list list (one zone per room line) and one display list of three 160B objects shared by every line; the ARM serves each line's 80 graphics bytes as MARIA reads them (`vcsInjectDmaData`). A 6502 loop in RAM reads the palette colours and the sound registers the ARM serves in each VBLANK, then the controllers, which the ARM snoops (`vcsSnoopRead`).
- Music: `music_init`, `music_frame`, `music_effect(MUSIC_EFFECT_FIRE / _BUMP)`; `game.audio` holds the six TIA sound registers each screen writes. The 7800 has the same TIA, so the song sounds the same.

## Kernel rules
- 2600: between two vcsLib calls the 6507 is already waiting: work is done only right after a `vcsSta3(WSYNC)` or in the overblank. `kernel()` and what it calls are in RAM (`RAM_FUNC`, `RAM_K`); `vcsJmp3()` once a line.
- 7800: the 6502 never runs from the cartridge after `vcsJmpToRam3(LOOP)`. `serve_frame()` is in RAM; between two lines it only has the time of MARIA's header reads, so the line buffers are packed in the VBLANK (`prepare_hero`).
- 7800 addresses: the Chameleon Cart sees A0-A12 only. What the console reads must never look (modulo $2000) like an address the ARM waits for; RAM lists stay out of $1800-$1FFF and $2280; served addresses have A12 set ($5000, $5100).
- The 7800 shows only the colours `game_colours()` returns, up to 12 (160B: four palettes of three). Colour bytes are NTSC values on both consoles.
- Calls to the loader are long (`#pragma long_calls` in `vcsLib.h`); only loader functions may stay undefined (the Makefile checks both).

## Folders
- `src/`: programs; `resources/gfx/`, `resources/music/`: `.vcsgfx`, `.vcsmusic`; `generated/`: what VCSIDE writes from them; `build/`: the build's. Set in the `.vcsproj` (`*_dir`).

## Build and run
- F7 runs `make`: `build/{{NAME}}.bin`, the 2600 side in Stella and Gopher2600.{{EXTRA_BUILDS}}
- The 7800 side: Build > Run on the Atari 7800 (Test7800). On real consoles only the Chameleon Cart runs it; the PlusCart's and UnoCart's loaders refuse an ELF that calls the 7800 functions.

## Do not touch
- `include/vcsLib.h`, `src/memcpy_long.S`{{GLUE}}, the Makefile's checks.
- `generated/gfx/hero.gfx.h`, `generated/music/music.h`: written by VCSIDE when `hero.vcsgfx` / `theme.vcsmusic` are saved.
