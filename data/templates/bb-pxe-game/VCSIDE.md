# {{NAME}}: notes for the assistant

VCSIDE The Game in batari Basic for the PXE kernel (batari Basic's ELF kernel, by ZackAttack): a room with a hero, two enemies, a shot, score and lives, and the TIA music played by C code linked into the kernel; it looks and plays like the DASM version. {{TITLE_NOTE}}

## Kernel rules
- `set kernel PXE`: one 60K program with no banks, which runs on the cartridge's ARM (Chameleon Cart, PlusCart, UnoCart; Stella 7 and Gopher2600). No `romsize`, no `set tv`: the kernel times the frame itself.
- 176 lines; `playerNy` counts lines from the top and is the sprite's top row. Sprites are drawn one line per row, so the `.vcsgfx` sprites have each row twice (the other versions draw two lines per row).
- Players 1 to 16 share the TIA's player 1: the enemies are `player1` and `player2`, both drawn on every frame. `collision(player1, ...)` is any of them: each enemy moves on every other frame (enemy 0 on even frames, enemy 1 on odd ones), so a wall touched after the frame is the one that moved; the shot hits the enemy closer to it (`nearestEnemy`). A dead enemy, the hidden shot: `y = 200`.
- `drawscreen` draws one frame per pass of `main`; the logic after it reads the collisions of the frame just drawn. `temp1` to `temp6` are lost in `drawscreen`.
- Colours: a table per sprite (`player0color:`, `player2color:`), `pfcolors:` per playfield row, `scorecolors:` per row of the score's digits, `COLUBL` for the ball (the shot). `NUSIZ0` bit 3 reflects the hero (facing left); `_NUSIZ1` and `NUSIZ2` are the enemies' sizes.
- `pfread` is not in this kernel's batari Basic.
- RAM: the game has `a` to `k`; `z` is `musicCommand`.

## Music
- `resources/music/theme.vcsmusic` exports `generated/music/music.h` (a TIA player in C) when it is saved. `pxe.c` is compiled and linked into the kernel's ELF after batari Basic builds the program: it wraps `vcsEndOverblank`, which the kernel calls once per frame, and there plays the music and writes AUDC, AUDF and AUDV of both channels through the kernel's CPU.
- The game asks for music with `musicCommand`: `musicShot`, `musicHit`, `musicLose` (1 + the effect's place in `theme.vcsmusic`), `musicTheme` (128 + the song), 255 stops. `pxe.c` reads it once per frame and sets it back to 0.

## Where things live
- Joystick and button: the start of `main` (the hero, then the shot, `shotSpeed`){{TITLE_WHERE}}.
- Hero: `placeHero`, `heroLook` (its frames: walking, standing, blinking); enemies: `newWave`, `moveEnemy`, `enemyCrawl1`/`2`; hits and walls: after `drawscreen`.
- Score: `score` (`scoreWhite`); lives: `pfscore1`.

## Folders
- `src/`: programs; `resources/gfx/`, `resources/music/`: `.vcsgfx`, `.vcsmusic`; `generated/`: what VCSIDE writes from them; `build/`: the build's. Set in the `.vcsproj` (`*_dir`).

## Build
- F7 compiles `src/main.bas` with batari Basic into `build/{{NAME}}.bin`, then links `pxe.c` and `music.h` into it with the Arm GNU toolchain (`arm-none-eabi-gcc`). F5 runs it.

## Do not touch
- `generated/music/music.h`: written by VCSIDE when `theme.vcsmusic` is saved.
- Lines between `rem vcsgfx` and `rem vcsgfx end`: VCSIDE rewrites them when the `.vcsgfx` is saved; change the `.vcsgfx` instead. `as player2` draws a sprite with another player.
