# {{NAME}}: notes for the assistant

A 4K DASM game from VCSIDE's game example: a title screen with a menu (PLAY, TV: NTSC / PAL60 / PAL) and a room with a hero, two enemies, a shot, score and lives.

## Kernel rules
- The TV system is chosen at run time (`System`: 0 NTSC, 1 PAL60, 2 PAL): 262, 262 or 312 lines. The blank lines are timed by `TIM64T` with `VblankTime` (43, 43, 73) and `OverscanTime` (36, 36, 65); the vertical blank's work must end before its timer, or the frame grows. It is the tight one (PAL colours are slow): `Animate` runs in the overscan for that reason.
- Colours are written for NTSC and turned into PAL ones by `ConvertColour` (slow): never call it between `WSYNC`s inside a kernel; precompute in the blank lines.
- `GameKernel`: two lines per pass; the enemies take turns on player 1 (30 Hz flicker, `Shown`).
- `TitleKernel`: "VCSIDE" is an asymmetric playfield (PF0-PF2 rewritten at cycles 28, 39 and 50); "THE GAME" (`Text48`) is 48-pixel text with its last writes at cycles 45, 48, 51 and 54, players at x 56 and 64, `VDELP0`/`VDELP1` on. Count cycles before changing either.
- `HMOVE` once a frame, in the vertical blank, after `PosObject`.

## Where things live
- Joystick and button: `StickPresses` and `ButtonPress` (title menu), `MoveHero` and `Shoot` (game).
- Hero: `MoveHero`, `Animate`; enemies: `MoveEnemy`, `NewWave`; hits and walls: `Collisions` (overscan, TIA collision registers).
- Sound: `MusicInit`, `MusicFrame` every frame, `MusicEffect` with `MUSIC_EFFECT_SHOT`, `MUSIC_EFFECT_HIT` or `MUSIC_EFFECT_LOSE`.

## Build
- F7 assembles `main.asm` with DASM (`-f3`) into `build/{{NAME}}.bin`, 4096 bytes (about 800 free). F5 runs it.

## Do not touch
- `*.gfx.asm` and `theme.music.asm`: written by VCSIDE when the `.vcsgfx` and `.vcsmusic` files are saved; change those instead.
- `2600.h` (register addresses).
