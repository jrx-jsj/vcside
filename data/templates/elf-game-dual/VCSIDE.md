# {{NAME}}: notes for the assistant

VCSIDE The Game in C on an ELF cartridge, for the Atari 2600 and, on a Chameleon Cart, the Atari 7800: one `.elf`. The 2600 side is VCSIDE The Game as it is; the 7800 side plays the same game (`game.c`, `menu.c`, the same song) with MARIA's graphics: wooden walls, a tiled floor, new sprites and a new cartridge on the title screen. `elf_main` (`src/main.c`) sends `ST_NTSC_7800` and `ST_PAL_7800` to `maria_game_run`, everything else to `tia_game_run`.

## Where things live
- `src/game.c`, `src/menu.c`, `src/screen.c`: the rules, the menu and the 2600's screens, shared with VCSIDE The Game; no bus calls.
- `src/tia_game.c`: the 2600 side, VCSIDE The Game's `main.c` with `tia_game_run` for `elf_main`.
- `src/maria_game.c`: the 7800 side. `src/art7800.c`: its pictures as text, a character a pixel (`art_game_letters` and `art_title_letters` give each character's slot, `art_*_colours` its NTSC colour).
- Sound: `game_sound_events` turns `game_frame`'s events into `music_effect(MUSIC_EFFECT_SHOT / _HIT / _LOSE)`; both sides play the song as samples on `AUDV0`.

## Kernel rules
- 2600: as VCSIDE The Game. Between two vcsLib calls the 6507 is already waiting: the kernels only copy values worked out in the overblank; kernels in RAM (`RAM_FUNC`, `RAM_K`); every visible line writes its sample.
- 7800: after `vcsJmpToRam3(CODE)` the 6502 runs a loop in the console's RAM. The picture is zones of 16 lines; MARIA reads line k of a zone of h lines at `GFX + (h - 1 - k) * 256`. `serve_frame` waits for MARIA's first read of the DLL, then on each line serves the picture's 80 bytes (`vcsInjectDmaData`) and two samples, and draws the next line (`draw_line`) in what is left of the line: only word copies and the sprites' pixels. Slower drawing loses the line and the frame. Texts, score and walls are drawn when they change, in the VBLANK (`prepare_title`, `prepare_score`, `prepare_pictures`).
- 7800 addresses: the Chameleon Cart sees A0-A12 only. Nothing the console reads may look (modulo $2000) like an address the ARM waits for; RAM lists stay out of $1800-$1FFF and $2280; served addresses have A12 set ($5000-$5F4F, $5F80, $5FF0).
- 7800 colours: 160B, 12 slots (four palettes of three) and black, one set for the game and one for the title; NTSC values, `colour()` makes them PAL. The 7800 menu has no HMOVE, and no PAL60 in SYSTEM.
- Calls to the loader are long (`#pragma long_calls` in `vcsLib.h`); only loader functions may stay undefined (the Makefile checks both).

## Folders
- `src/`: programs; `resources/gfx/`, `resources/music/`: `.vcsgfx`, `.vcsmusic`; `generated/`: what VCSIDE writes from them; `build/`: the build's. Set in the `.vcsproj` (`*_dir`).

## Build and run
- F7 runs `make`: `build/{{NAME}}.bin`, the 2600 side in Stella and Gopher2600.{{EXTRA_BUILDS}}
- The 7800 side: Build > Run on the Atari 7800 (Test7800). On real consoles only the Chameleon Cart runs it; the PlusCart's and UnoCart's loaders refuse an ELF that calls the 7800 functions.

## Do not touch
- `include/vcsLib.h`, `src/memcpy_long.S`{{GLUE}}, the Makefile's checks.
- `generated/gfx/*.gfx.h` and `generated/music/music.h`: written by VCSIDE when the `.vcsgfx` and `theme.vcsmusic` are saved.
