# {{NAME}}: notes for the assistant

An example batari Basic program for a {{FORMAT}} cartridge, from VCSIDE: a square moved with the joystick, a playfield room, and the TIA music player.

## Kernel rules
- batari Basic's standard kernel draws the screen in `drawscreen`, once per pass of the `main` loop; the game logic runs before and after it.
- `set romsize {{ROMSIZE}}`; with a 50 Hz TV standard, `set tv pal` too. From 8k on, batari Basic keeps its kernel in the last bank and the program starts in bank 1: when bank 1 fills up, `bank 2` (and so on) starts the next, and `goto label bank2` and `gosub label bank2` reach code in another bank.
- The music player uses the variables `h` to `z`: the program uses only `a`, `b` and `c` (`colour`, `button`, `touching`).

## Where things live
- Joystick and button: `joy0left`/`right`/`up`/`down`, `joy0fire` in `main`.
- The square: `player0`, `player0x`, `player0y`; the room: `playfield:`; the wall: `collision(player0, playfield)` after `drawscreen`.
- Music: `asm` blocks call `MusicInit` (at `start`), `MusicFrame` (every frame) and `MusicEffect` with `MUSIC_EFFECT_BLIP` or `MUSIC_EFFECT_BUMP`; the player comes from `inline music.asm` at the end.

## Folders
- `src/`: programs; `resources/gfx/`, `resources/music/`: `.vcsgfx`, `.vcsmusic`; `generated/`: what VCSIDE writes from them; `build/`: the build's. Set in the `.vcsproj` (`*_dir`).

## Build
- F7 compiles `src/main.bas` with batari Basic into `build/{{NAME}}.bin`. F5 runs it.

## Do not touch
- `generated/music/music.asm`: written by VCSIDE when `theme.vcsmusic` is saved. Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `MusicPlay` with `MUSIC_SONG_<NAME>` in A starts another song, `MusicStop` stops it.
- Lines between `rem vcsgfx` and `rem vcsgfx end`, if any: VCSIDE rewrites them.
