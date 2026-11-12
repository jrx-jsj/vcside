# {{NAME}}: notes for the assistant

A batari Basic program for a {{FORMAT}} cartridge, from VCSIDE's template: a square moved with the joystick, a playfield room, and the TIA music player.

## Kernel rules
- batari Basic's standard kernel draws the screen in `drawscreen`, once per pass of the `main` loop; the game logic runs before and after it.
- `set romsize {{ROMSIZE}}`; with a 50 Hz TV standard, `set tv pal` too. From 8k on, batari Basic keeps its kernel in the last bank and the program starts in bank 1: when bank 1 fills up, `bank 2` (and so on) starts the next, and `goto label bank2` and `gosub label bank2` reach code in another bank.
- The music player uses the variables `h` to `z`: the program uses only `a`, `b` and `c` (`colour`, `button`, `touching`).

## Where things live
- Joystick and button: `joy0left`/`right`/`up`/`down`, `joy0fire` in `main`.
- The square: `player0`, `player0x`, `player0y`; the room: `playfield:`; the wall: `collision(player0, playfield)` after `drawscreen`.
- Music: `asm` blocks call `MusicInit` (at `start`), `MusicFrame` (every frame) and `MusicEffect` with `MUSIC_EFFECT_BLIP` or `MUSIC_EFFECT_BUMP`; the player comes from `inline theme.music.asm` at the end.

## Build
- F7 compiles `main.bas` with batari Basic into `main.bas.bin`. F5 runs it.

## Do not touch
- `theme.music.asm`: written by VCSIDE when `theme.vcsmusic` is saved.
- Lines between `rem vcsgfx` and `rem vcsgfx end`, if any: VCSIDE rewrites them.
