# {{NAME}}: notes for the assistant

A 32K CDFJ program from VCSIDE's CDFJ example: the 6507 draws (`main.asm`), the game logic and the music are C run by the cartridge's ARM (`game.c`).

## Kernel rules
- Fast fetch is on: `lda #n` with n from 0 to `$23` reads data stream n instead of loading n. Load constants with `ldx #` or `ldy #`.
- Every line copies the voices' sum to the TIA (`lda #AMPLITUDE` / `sta AUDV0`, `AUDC0` = 0).
- `CallArm` writes `SWCHA` and `INPT4` to the display data and writes `CALLFN`: the 6507 waits there while the C code runs (sound goes on).
- `HMOVE` once a frame, in the vertical blank, after `PosObject`.
- The C code must be Thumb only (the Makefile stops on ARM code, which emulators do not run).

## Where things live
- Joystick and button: read by `CallArm`, used by `frame()` in `game.c`.
- What the 6507 and the ARM share: `layout.h` (offsets in the display data, one `#define NAME number` per line, decimal).
- Music: `music_init`, `music_frame`, `music_effect(MUSIC_EFFECT_FIRE / _BUMP)` in `game.c`; "bump" plays on the TIA's channel 1 from `LAYOUT_TIA`. A song with a TIA track (*TIA track* in the music editor) plays it there too, through the same `music_tia`, when no effect sounds there.

## Folders
- `src/`: programs; `resources/gfx/`, `resources/music/`: `.vcsgfx`, `.vcsmusic`; `generated/`: what VCSIDE writes from them; `build/`: the build's. Set in the `.vcsproj` (`*_dir`).

## Build
- F7 runs `make`: `build/{{NAME}}.bin` (32K). Needs DASM, `arm-none-eabi-gcc` and `cdfj_driver.bin` (CDFJ's 2K driver, not included). PAL: `make DASMFLAGS=-DPAL=1`.

## Do not touch
- `cdfj_driver.bin`; `build/layout.asm` (made from `layout.h`); `generated/music/music.h` (written when `theme.vcsmusic` is saved). Every `.vcsmusic` of that folder exported the same way goes into it, with one player: `music_play(MUSIC_SONG_<NAME>)` starts another song, `music_stop()` stops it.
