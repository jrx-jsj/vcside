# {{NAME}}: notes for the assistant

VCSIDE The Game in DASM, on a {{FORMAT}} cartridge: a room with a hero, two enemies, a shot, score and lives. Two switches at the top of `main.asm` say what comes with it: `TITLE_SCREEN` (1: a title screen with a menu, PLAY and TV: NTSC / PAL60 / PAL; 0: the game starts at once and starts again after the last life) and `MUSIC` (1: the TIA music player and `theme.vcsmusic`; 0: the three effects alone, in `MusicEffect`/`MusicFrame` without the player). The title's code and data are inside `IF TITLE_SCREEN`.

## Kernel rules
- The TV system is chosen at run time (`System`: 0 NTSC, 1 PAL60, 2 PAL; it starts as `DEFAULT_SYSTEM`, and only the title's menu changes it): 262, 262 or 312 lines. The blank lines are timed by `TIM64T` with `VblankTime` (43, 43, 73) and `OverscanTime` (36, 36, 65); the vertical blank's work must end before its timer, or the frame grows. It is the tight one: `Animate` (twelve colours through `ConvertColour`) runs in the overscan for that reason.
- Colours are written for NTSC and turned into PAL ones by `ConvertColour`, which looks each one up in `NtscToPal` (the closest PAL colour to each NTSC one; edit an entry to retune it). Never call it between `WSYNC`s inside a kernel: precompute in the blank lines, as `CartColoursForTv` does for the cartridge's row colours (`CartColours`).
- RAM from `Shared` on is the title's (`CartColours`) or the game's (enemies, shot, score, lives): `StartGame` clears it. `TitleEnd` and `GameEnd` must stay below $F6 for the stack.
- `GameKernel`: two lines per pass; with both enemies alive they take turns on player 1 (30 Hz flicker, `Shown`); with one, it is drawn on every frame. Each enemy moves on every other frame either way (`MoveEnemy`).
- `EnemyRow` = $F0 means no enemy on the frame: `Collisions` then leaves the enemies alone (`StartGame` sets it, since its frame is drawn before `GameLogic` has run).
- `TitleKernel`: the cartridge (`Cartridge`) is an asymmetric playfield with a colour per row (COLUPF, PF0-PF2 and PF0-PF2 again at cycles 7-51) and the missiles as the label's grey sides (pixels 36 and 116); "THE GAME" (`Text48`) is 48-pixel text with its last writes at cycles 44, 47, 50 and 53, players on pixels 55 and 63, `VDELP0`/`VDELP1` on. Count cycles before changing either.
- `HMOVE` once a frame, in the vertical blank, after `PosObject`.

## Where things live
- Joystick and button: `StickPresses` and `ButtonPress` (title menu), `MoveHero` and `Shoot` (game).
- Hero: `MoveHero`, `Animate`; enemies: `MoveEnemy`, `NewWave`; hits and walls: `Collisions` (overscan, TIA collision registers).
- Sound: `MusicInit`, `MusicFrame` every frame, `MusicEffect` with `MUSIC_EFFECT_SHOT`, `MUSIC_EFFECT_HIT` or `MUSIC_EFFECT_LOSE`.

## Build
- F7 assembles `main.asm` with DASM (`-f3`) into `build/{{NAME}}.bin`, {{ROM_BYTES}} bytes. With the title and the music, 4K has about 350 bytes free; on F8, F6 and F4 the game is all in bank 0 and the other banks are empty (the end of `main.asm` closes every bank: the vectors switch to bank 0 whatever bank the cartridge starts in).

## Do not touch
- `*.gfx.asm` and `music.asm`: written by VCSIDE when the `.vcsgfx` and `.vcsmusic` files are saved; change those instead. Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `MusicPlay` with `MUSIC_SONG_<NAME>` in A starts another song, `MusicStop` stops it.
- `2600.h` (register addresses).
