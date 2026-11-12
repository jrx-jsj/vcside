# {{NAME}}: notes for the assistant

A 32K DPC+ program from VCSIDE's empty DPC+ template: the ARM driver, the cartridge's layout (six banks, the display data, the frequency table) and a frame loop in bank 5 that draws a colour per line. Keep these notes true as the game grows.

## Kernel rules
- The frame: VSYNC (3 lines), the vertical blank, `VISIBLE_LINES` visible lines, the overscan. Totals: 262 lines (NTSC, PAL60) or 312 (PAL).
- Every line copies the voices' sum to the TIA: `SOUND` (`lda AMPLITUDE` / `sta AUDV0`, `AUDC0` = 0). Code that takes longer than a line does it in between, or the music stops.
- The blank lines are timed by `TIM64T` and polled a line at a time with `bit TIMINT` (D7: the timer ran out). Logic goes before the poll and must end before the timer.
- Fast fetch (`FASTFETCH` = 0) makes `lda #n` with n < $28 read DPC+ register n: turn it on only around kernels, and load constants there with `ldx #`/`ldy #`.
- `HMOVE` once a frame, in the vertical blank, after positioning.

## Where things live
- Bank 5: `Reset`, `StartFrame`, the kernel. Banks 0-4: empty; each ends with `BANK_END`, which goes to bank 5 on reset. The first 128 bytes of every bank are DPC+'s registers: code starts after them.
- The display data (`ORG $7000`, `RORG $0000`: labels are offsets in it): waveforms (32 bytes, 32-aligned) and data for the fetchers.
- The frequency table (`ORG $8000`): one 4-byte entry per note.
- Joystick, button, sound: not read or played yet.

## Build
- F7 assembles `main.asm` with DASM (`-f3`) into `build/{{NAME}}.bin`, 32768 bytes. F5 runs it.

## Do not touch
- `DPCplus.arm`: DPC+'s ARM driver, copied from batari Basic.
- `2600.h` and the register addresses at the top of `main.asm`.
