# {{NAME}}: notes for the assistant

VCSIDE The Game on a 32K CDFJ cartridge: a title screen with a menu (PLAY, TV: NTSC / PAL60 / PAL), then a room with a hero, two enemies, a shot, score and lives. The game logic and the music are C run by the cartridge's ARM (`game.c`); the 6507 only draws (`main.asm`). The music is three sampled voices.

## Kernel rules
- Fast fetch is on: `lda #n` with n from 0 to `$23` reads data stream n instead of loading n. Load constants with `ldx #` or `ldy #`.
- Every line copies the voices' sum to the TIA (`SOUND`: `lda #AMPLITUDE` / `sta AUDV0`, `AUDC0` = 0).
- `CallArm` writes the inputs (`LAYOUT_FUNCTION` to `LAYOUT_CXPPMM`) and writes `CALLFN`: the 6507 waits there while the C code runs (sound goes on).
- After `CallArm`, the 6507 reads what `send_registers` in `game.c` sent, in the same order, with `lda DSCOMM`: change both together.
- The TV system is chosen at run time (`System`: 0 NTSC, 1 PAL60, 2 PAL): 262 or 312 lines, `VblankTime`/`OverscanTime`, polled with `bit TIMINT`. Measured: change them only by measuring again.
- `HMOVE` once a frame, in the vertical blank, after `PosObject`.
- The C code must be Thumb only (the Makefile stops on ARM code, which emulators do not run).

## Where things live
- `game.c`: `start()` (power on), `frame()` (a frame: menu or game logic, collisions, music), `title_streams`/`game_streams` (what the kernels draw, a byte per line or pass in the buffers).
- `main.asm`: `TitleKernel` (the cartridge, THE GAME in 48 pixels, the menu), `GameKernel` (the room, the hero, the enemy, the shot as the ball, score and lives).
- `layout.h`: what the 6507 and the ARM share (offsets in the display data and data stream numbers, one `#define NAME number` per line, decimal).
- Effects: `music_effect(MUSIC_EFFECT_SHOT / _HIT / _LOSE)`; "hit" plays on the TIA's channel 1 (`music_tia`). A song with a TIA track (*TIA track* in the music editor) plays it there too, through the same `music_tia`, when no effect sounds there.

## Folders
- `src/`: programs; `resources/gfx/`, `resources/music/`: `.vcsgfx`, `.vcsmusic`; `generated/`: what VCSIDE writes from them; `build/`: the build's. Set in the `.vcsproj` (`*_dir`).

## Build
- F7 runs `make`: `build/{{NAME}}.bin` (32K). Needs DASM, `arm-none-eabi-gcc` and `cdfj_driver.bin` (CDFJ's 2K driver, not included).

## Do not touch
- `cdfj_driver.bin`; `build/layout.asm` (made from `layout.h`); `*.gfx.h` and `generated/music/music.h` (written when the `.vcsgfx` and `.vcsmusic` files are saved); `start.S`, `arm.ld`, `cdfj.h`. Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `music_play(MUSIC_SONG_<NAME>)` starts another song, `music_stop()` stops it.
