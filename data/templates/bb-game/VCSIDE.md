# {{NAME}}: notes for the assistant

VCSIDE The Game in batari Basic, on a {{FORMAT}} cartridge, in the standard kernel: a room with a hero, two enemies, a shot, score and lives, and the TIA music; it looks and plays like the DASM version. {{TITLE_NOTE}}

## Kernel rules
- `drawscreen` draws one frame per pass of `main`; the logic before it sets what the frame shows, the logic after it reads the collisions of the frame just drawn (`collision(...)`). `temp1` to `temp6` are lost in `drawscreen`.
- `set romsize {{ROMSIZE}}`; with a 50 Hz TV standard, `set tv pal` too (batari Basic chooses the timing when it compiles: there is no menu for the TV system). From 8k on, the kernel is in the last bank and the program in bank 1; `bank 2` and so on start more banks.
- RAM: the music player has `h` to `z`; the game has `a` to `g` and `var44` to `var47` (the playfield's twelfth row, never drawn; only vertical scrolling uses it). `const pfscore = 1` takes `aux3` to `aux5` for the lives (`pfscore1`, three dots; each life lost is `/ 4`).
- `set kernel_options player1colors playercolors pfheights`: a colour per row for both players (`player0color:`, `player1color:`). That takes both missiles, so the shot is the ball (`ballx`, `bally`; `CTRLPF = $21` makes it 4 pixels wide), in the playfield's colour. batari Basic takes `playercolors` only with `pfcolors` or `pfheights`: `pfheights:` at `start` makes every playfield row 8 double lines tall, as in the `.vcsgfx`.
- The playfield has one colour: the lives' dots are drawn last with `pfscorecolor` and leave it in COLUPF, so `pfscorecolor` is the playfield's colour (the title's at `title`, the room's at `newGame`), written there when the project was made: after changing that colour in the `.vcsgfx`, change it too.
- `NUSIZ0`, `NUSIZ1` and `REFP0` are set before every `drawscreen`, because the score changes them (the title's cartridge is both players at quad width, `$07`).
- The enemies: both alive take turns on player 1 (each drawn every other frame, 30 Hz flicker); one alone is drawn on every frame. Either way each takes a diagonal step on every other frame (`moveEnemy`). Flag bit 4 says which was drawn, for the collisions after `drawscreen`. The second enemy's colour is the `player1color:` table after the enemy's frames in `main`, not in `enemy.vcsgfx`.
- Walls: the hero goes back to where it was before the frame's move (`oldHeroX`, `oldHeroY`); after touching one (flag bit 7), held diagonally it moves on one axis a frame, so it slides along the wall. An enemy goes back and turns on one axis chosen at random (`rand`).
- The `.vcsgfx` colours are those of the project's TV standard: a PAL or PAL60 project got the template's NTSC colours turned into the closest PAL ones. `scorecolor` is a grey, the same in both.

## Where things live
- Joystick and button: the start of `main` (the hero, then the shot, `shotSpeed`){{TITLE_WHERE}}.
- Hero: `placeHero`, `heroLook` (its frames: walking, standing, blinking); enemies: `newWave`, `moveEnemy`, `enemyCrawl1`/`2`; hits and walls: after `drawscreen`.
- Score: `score`; lives: `pfscore1`.
- Sound: `MusicInit` at `start`, `MusicFrame` before every `drawscreen`, `MusicEffect` with `MUSIC_EFFECT_SHOT`, `MUSIC_EFFECT_HIT` or `MUSIC_EFFECT_LOSE`; `inline theme.music.asm` at the end.

## Build
- F7 compiles `main.bas` with batari Basic into `main.bas.bin`. F5 runs it.

## Do not touch
- `theme.music.asm`: written by VCSIDE when `theme.vcsmusic` is saved.
- Lines between `rem vcsgfx` and `rem vcsgfx end`: VCSIDE rewrites them when the `.vcsgfx` is saved; change the `.vcsgfx` instead.
