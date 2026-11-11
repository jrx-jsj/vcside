# {{NAME}}: notes for the assistant

A 4K DASM program from VCSIDE's animated sprite example: a hero that walks left and right, with the TIA music player.

## Kernel rules
- The frame: VSYNC, the vertical blank and the overscan timed by `TIM64T`/`INTIM`; `VISIBLE_LINES` visible lines (262 or 312 in all).
- The kernel draws a line pair per pass (`PAIRS = VISIBLE_LINES / 2`): the first line writes `GRP0`/`COLUP0` from `CurGfx`/`CurCol`, worked out on the pass before; the frame and colour come through the pointers `GfxPtr` and `ColPtr`. Two `WSYNC` per pass.
- `HMOVE` once a frame, in the vertical blank, after `PosObject` (not on every line).

## Where things live
- Joystick: `Joystick` (`SWCHA`). Fire button: `Sounds` (`INPT4`, bit 7 = 0 when down).
- The hero: `Animate` (animation tables) and the kernel; `PosObject` places it.
- Music: `MusicInit` at power on, `MusicFrame` once a frame, `MusicEffect` with `MUSIC_EFFECT_JUMP` or `MUSIC_EFFECT_BUMP` in A (from `Sounds`). Its RAM is `MusicRam` (19 bytes).

## Build
- F7 assembles `main.asm` with DASM (`-f3`) into `build/{{NAME}}.bin`, 4096 bytes. F5 runs it.

## Do not touch
- `hero.gfx.asm` and `theme.music.asm`: written by VCSIDE when `hero.vcsgfx` or `theme.vcsmusic` is saved; change those instead.
- `2600.h` (register addresses).
