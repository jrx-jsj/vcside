# {{NAME}}: notes for the assistant

An empty batari Basic program for a {{FORMAT}} cartridge, from VCSIDE's template: only the frame loop.

## Kernel rules
- batari Basic's standard kernel draws the screen in `drawscreen`, once per pass of the `main` loop; the game logic runs before and after it.
- `set romsize {{ROMSIZE}}`; with a 50 Hz TV standard, `set tv pal` too. From 8k on, batari Basic keeps its kernel in the last bank and the program starts in bank 1: when bank 1 fills up, `bank 2` (and so on) starts the next, and `goto label bank2` and `gosub label bank2` reach code in another bank.
- The variables `a` to `z` are free; `temp1` to `temp6` are lost in every `drawscreen`. The playfield uses `var0` to `var47`.
- Colour and size registers (`COLUP0`, `NUSIZ0`, `REFP0`...) are set before every `drawscreen`: the score changes some of them.

## Where things live
- `main`: the frame loop.

## Build
- F7 compiles `main.bas` with batari Basic into `main.bas.bin`. F5 runs it.

## Do not touch
- Lines between `rem vcsgfx` and `rem vcsgfx end`, if any: VCSIDE rewrites them when the `.vcsgfx` is saved.
