# {{NAME}}: notes for the assistant

A DASM program for a {{FORMAT}} cartridge, from VCSIDE's empty template: a frame loop and nothing else yet. Keep these notes true as the game grows.

## Kernel rules
- The frame: VSYNC (3 lines), the vertical blank, `VISIBLE_LINES` visible lines, the overscan. Totals: 262 lines (NTSC, PAL60) or 312 (PAL).
- The vertical blank and the overscan are timed by the RIOT: `TIM64T` is loaded with `(LINES*76-14)/64`, then `INTIM` is polled. Game logic goes before the poll and must end before the timer runs out.
- The kernel writes once per line after `sta WSYNC`: one `WSYNC` per visible line, exactly `VISIBLE_LINES` of them.
- No `HMOVE` yet.

## Where things live
- `main.asm`: everything (RAM at `$80`, code at `{{ROM_START}}`, vectors at `$FFFC`).
- Joystick, button, sound: not read or played yet.
- `2600.h`: the TIA and RIOT register names.

## Build
- F7 assembles `main.asm` with DASM (`-f3`) into `build/{{NAME}}.bin`, {{ROM_BYTES}} bytes. F5 runs it.

## Do not touch
- `2600.h` (register addresses).
