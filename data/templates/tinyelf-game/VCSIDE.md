# {{NAME}}: notes for the assistant

VCSIDE The Game as a TinyELF Basic program, from VCSIDE's template. TinyELF Basic (Al_Nafuur, built on Stefan Lenz's Basic2, with Omegamatrix's 36-character text kernel) is a BASIC machine that runs as an ELF game on the PlusCart and the UnoCart. VCSIDE builds this program into its ROM: at power-on the program is entered as if typed, then run.

## The game
- A title screen (THE GAME, the hero and two enemies, the song); fire, SPACE or RETURN starts a game.
- The joystick or the cursor keys move the hero (a key keeps it walking, RETURN stops it); fire or SPACE shoots the way it faces. Walls and blocks stop the hero and the shot; the two enemies walk diagonally and bounce off them. A shot enemy scores a point; when both are gone, two more come. Touching one costs a life; after the third, GAME OVER and back to the title.
- The same rules, room, sprites, song and effects as the other VCSIDE The Game templates (`room.vcsgfx`, `hero.vcsgfx`, `enemy.vcsgfx`, `title.vcsgfx`, `theme-tia.vcsmusic`), written into the listing as `DATA`: there are no resource files.

## How it draws (GRAPHICS 8, with its three-row text window for the score and the lives)
- Video RAM: `4096 + line*20 + byte`. Lines 0-131 are the room (11 text rows of 12 lines; the kernel adds a blank line between rows). Bytes 0-17 are 8 pixels each, bit 7 on the left. The kernel draws every other byte column on alternate frames, so sprites flicker at 30 Hz, like its text.
- Byte 18 of a line holds byte 2's left five pixels for the kernel: after writing byte 2, write `PEEK(byte 2) AND 248` into byte 18 (lines 760-762).
- The sprites (`GL`, `GR`) are each frame shifted by 0, 2, 4 and 6 pixels into a left and a right byte; the hero's are also mirrored. A sprite's routine (lines 700-716) writes the new bytes over the old ones and clears only what the move uncovered. Sprites one byte apart overwrite each other's bytes, which is why the hero is redrawn while an enemy is near.
- The walls are the playfield shadow (`128 + row*8`: COLUPF, CTRLPF, PF0-PF2 twice), mirrored (CTRLPF 5). In GRAPHICS 8 the kernel shows the left half's bytes on both sides, so the room has to be symmetric. `WM` is the same room as a grid (40 playfield columns by 11 rows) for the collisions, and `PC(x+1)` the playfield column of pixel x.
- Colours: one per text row (`256 + row`), so actors on the same row share a colour; a background colour per line (`1024 + line`).
- Sound: the song is a row of AUDC/AUDF/AUDV for both channels every 100 ms (`MILLIS(100)`); an effect takes channel 1 for a few 50 ms steps; the lose sound plays with `DELAY`.

## Speed
- The interpreter runs a budget of tokens (332) between two frames, the same on the cartridge and in Gopher2600: a turn of the game loop takes about 75 ms (4 or 5 frames), mostly the `POKE`s of the sprites. Fewer lines to draw or fewer tests per turn make it faster; a bigger budget (`POKE 653,…: POKE 652,…`) can make the TV lose its sync.
- Only the first two letters of a name count (`HX`, `EX`…), and a name must not contain a keyword. Comparisons bind weaker than `AND` and `OR`: write `(A=1) AND (B=2)`.
- Arrays start at 1.

## Build
- F7 compiles TinyELF Basic's sources (VCSIDE's `data/tinyelf`, GPL-3) with the program in it into `build/{{NAME}}.bin`. It needs the Arm GNU Toolchain with its C library (newlib); Homebrew's `arm-none-eabi-gcc` has none.
- F5 runs it in Gopher2600 with `-keyportari ASCII` (Project > Emulator options): the computer's keyboard is the KeyPortari. Stella cannot run it (Cortex-M4 code).
- Size: TinyELF and this listing make about 93 KB of the loader's 96 KB; long comments can push it over.

## Do not touch
- Nothing is generated: every file is the user's.
