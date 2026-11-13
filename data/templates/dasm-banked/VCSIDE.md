# {{NAME}}: notes for the assistant

A DASM program for a {{FORMAT}} cartridge ({{BANKS}} banks of 4K), from VCSIDE's empty template: a frame loop in bank 0 that calls into bank 1 once a frame. Keep these notes true as the game grows.

## Kernel rules
- The frame: VSYNC (3 lines), the vertical blank, `VISIBLE_LINES` visible lines, the overscan. Totals: 262 lines (NTSC, PAL60) or 312 (PAL).
- The vertical blank and the overscan are timed by the RIOT: `TIM64T` is loaded with `(LINES*76-14)/64`, then `INTIM` is polled. Game logic goes before the poll and must end before the timer runs out.
- The kernel writes once per line after `sta WSYNC`: one `WSYNC` per visible line, exactly `VISIBLE_LINES` of them.

## Banks
- Each bank is assembled at `$F000-$FFFF` (`ORG` for its place in the file, `RORG $F000`). Touching `BANK0_HOTSPOT+n` shows bank n; the next instruction comes from the new bank at the next address.
- `BANK_END n` closes every bank with the same code at `$FFE0`: the vectors switch to bank 0 and jump to `Reset`, whatever bank the cartridge starts in. A bank's code must end before `$FFE0` (DASM stops with an error if it does not).
- Calling another bank: `CallBank1` (`$FFE6` in bank 0) switches to bank 1, which runs `jsr FrameLogic` at `$FFE9` and switches back at `$FFEC`; bank 0 returns with the `rts` at `$FFEF`. Another call needs its own trampoline at free addresses below `$FFF4`, in both banks.
- RAM is the same in every bank; the stack's return addresses are only valid in the bank that pushed them.

## Where things live
- Bank 0: `Reset`, `StartFrame` and the kernel. Bank 1: `FrameLogic`. Banks 2 on: empty.
- Joystick, button, sound: not read or played yet.
- `2600.h`: the TIA and RIOT register names.

## Folders
- `src/`: programs; `resources/gfx/`, `resources/music/`: `.vcsgfx`, `.vcsmusic`; `generated/`: what VCSIDE writes from them; `build/`: the build's. Set in the `.vcsproj` (`*_dir`).

## Build
- F7 assembles `src/main.asm` with DASM (`-f3`) into `build/{{NAME}}.bin`, {{ROM_BYTES}} bytes. F5 runs it.

## Do not touch
- `2600.h` (register addresses).
