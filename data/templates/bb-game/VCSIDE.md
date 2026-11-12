# {{NAME}}: notes for the assistant

VCSIDE The Game in batari Basic, on a {{FORMAT}} cartridge, in the standard kernel: a room with a hero, two enemies, a shot, score and lives, and the TIA music. {{TITLE_NOTE}}

## Kernel rules
- `drawscreen` draws one frame per pass of `main`; the logic before it sets what the frame shows, the logic after it reads the collisions of the frame just drawn (`collision(...)`). `temp1` to `temp6` are lost in `drawscreen`.
- `set romsize {{ROMSIZE}}`; with a 50 Hz TV standard, `set tv pal` too (batari Basic chooses the timing when it compiles: there is no menu for the TV system). From 8k on, the kernel is in the last bank and the program in bank 1; `bank 2` and so on start more banks.
- RAM: the music player has `h` to `z`; the game has `a` to `g` and `var44` to `var47` (the playfield's twelfth row, never drawn; only vertical scrolling uses it). `const pfscore = 1` takes `aux3` to `aux5` for the lives (`pfscore1`, three dots; each life lost is `/ 4`).
- `set kernel_options pfcolors`: a colour per playfield row (`pfcolors:`), except the top row, which keeps `pfscorecolor` (the lives' dots, drawn last). So `pfscorecolor` is set to the top row's colour (the title's at `title`, the room's at `newGame`), written there when the project was made: after changing that row's colour in the `.vcsgfx`, change it too.
- Missile 0 is the shot: no `kernel_options` that take it (`playercolors`, `no_blank_lines`, `readpaddle`). The players have one colour (the first row's); `NUSIZ0` and `REFP0` are set before every `drawscreen`, because the score changes them.
- The enemies: both alive take turns on player 1 (each drawn every other frame, 30 Hz flicker); one alone is drawn on every frame, so it moves twice as fast. Flag bit 4 says which was drawn, for the collisions after `drawscreen`. Each moves on x for two frames and on y for the next two (`moveEnemy`), so a playfield collision is on the axis it moved on: that axis turns.
- The `.vcsgfx` colours are those of the project's TV standard: a PAL or PAL60 project got the template's NTSC colours turned into the closest PAL ones. `scorecolor` is a grey, the same in both.

## Where things live
- Joystick and button: the start of `main` (hero, `facing`, the shot from `shootRight` on){{TITLE_WHERE}}.
- Hero: `placeHero`, `heroLook` (its frames: walking, standing, blinking); enemies: `newWave`, `moveEnemy`, `enemyCrawl1`/`2`; hits and walls: after `drawscreen`.
- Score: `score`; lives: `pfscore1`.
- Sound: `MusicInit` at `start`, `MusicFrame` before every `drawscreen`, `MusicEffect` with `MUSIC_EFFECT_SHOT`, `MUSIC_EFFECT_HIT` or `MUSIC_EFFECT_LOSE`; `inline theme.music.asm` at the end.

## Build
- F7 compiles `main.bas` with batari Basic into `main.bas.bin`. F5 runs it.

## Do not touch
- `theme.music.asm`: written by VCSIDE when `theme.vcsmusic` is saved.
- Lines between `rem vcsgfx` and `rem vcsgfx end`: VCSIDE rewrites them when the `.vcsgfx` is saved; change the `.vcsgfx` instead.
