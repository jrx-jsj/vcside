# {{NAME}}: notes for the assistant

An ACE cartridge game in C (PlusCart, UnoCart) from VCSIDE's template: the ARM (Cortex-M4 with FPU) feeds the 6507 every instruction of the visible lines through VCSIDE's bus library (`include/ace.h`, `src/ace.c`); during the overblank the 6507 runs VSYNC and the blank lines from its own RAM and the ARM runs the game logic.

## Kernel rules
- Between two `ace_` calls the 6507 is already waiting: work is done only right after `ace_wsync()` or in the overblank (between `ace_overblank_start()` and `ace_overblank_end()`).
- `ace_jmp()` once a line, so the 6507's code stays at the same place.
- Every visible line writes its sample to `AUDV0`; the first 31 lines also write the overblank's samples to `$E0`-`$FE`.
- Visible lines: `lines`, the frame's lines minus the 63 of the overblank, set from the project's TV standard when it was made (ACE passes no system to the game).
- The start code (`src/start.S`) must follow the 40-byte header (`ace.ld`; the Makefile checks it).

## Where things live
- `main()` in `src/main.c`: the game loop. Joystick, switches, button: read by `kernel()` (`swcha`, `swchb`, `inpt4`). RESET: `leave()` and back to the cartridge's menu.
- Music: `music_init`, `music_frame(lines + MUSIC_OVERBLANK_LINES)`, `music_effect(MUSIC_EFFECT_FIRE / _BUMP)`; "bump" plays on the TIA's channel 1 from `music_tia`.

## Build
- F7 runs `make`: `build/{{NAME}}.ace` and `build/ACE_debugging.elf` (for Gopher2600's debugger). Runs in Gopher2600 only (Stella has no ACE).

## Do not touch
- `include/ace.h`, `src/ace.c`, `src/start.S`, `src/memory.c`, `ace.ld` (the bus library, start code and header).
- `src/music.h`: written by VCSIDE when `theme.vcsmusic` is saved. Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `music_play(MUSIC_SONG_<NAME>)` starts another song, `music_stop()` stops it.
