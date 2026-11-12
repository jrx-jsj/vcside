# {{NAME}}: notes for the assistant

A 32K CDFJ program from VCSIDE's empty CDFJ template: the ARM driver, the cartridge's layout (the driver, the C code from $0800, the 6507's bank 6 at $7000, the display data as the cartridge's RAM), a frame loop that calls the C code once a frame and draws a colour per line. Keep these notes true as the game grows.

## Kernel rules
- The frame: VSYNC (3 lines), the vertical blank, `VISIBLE_LINES` visible lines, the overscan. Totals: 262 lines (NTSC, PAL60) or 312 (PAL).
- Fast fetch is on: `lda #n` with n from 0 to `$23` reads data stream n instead of loading n. Load constants with `ldx #` or `ldy #`.
- Every line copies the voices' sum to the TIA (`SOUND`: `lda #AMPLITUDE` / `sta AUDV0`, `AUDC0` = 0). Code that takes longer than a line does it in between, or the sound stops.
- The blank lines are timed by `TIM64T` and polled a line at a time with `bit TIMINT`. `CallArm` and the logic go before the poll and must end before the timer.
- `CallArm` writes `LAYOUT_FUNCTION`, `SWCHA` and `INPT4` to the display data and writes `CALLFN`: the 6507 waits there while the C code runs (sound goes on).
- `HMOVE` once a frame, in the vertical blank, after positioning.
- The C code must be Thumb only (the Makefile stops on ARM code, which emulators do not run). Variables with initial values are not supported: set them in `start()`.

## Where things live
- `game.c`: `start()` (power on: clears the variables, silences the voices), `frame()` (once a frame, the game's logic).
- `layout.h`: what the 6507 and the ARM share (offsets in the display data, one `#define NAME number` per line, decimal).
- Joystick and button: read by `CallArm`, given to `frame()`. Sound: the voices are silent.

## Build
- F7 runs `make`: `build/{{NAME}}.bin` (32K). Needs DASM, `arm-none-eabi-gcc` and `cdfj_driver.bin` (CDFJ's 2K driver, not included).

## Do not touch
- `cdfj_driver.bin`; `build/layout.asm` (made from `layout.h`); `start.S`, `arm.ld`, `cdfj.h`.
