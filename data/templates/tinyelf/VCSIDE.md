# {{NAME}}: notes for the assistant

A TinyELF Basic program, from VCSIDE's template. TinyELF Basic (Al_Nafuur, built on Stefan Lenz's Basic2, with Omegamatrix's 36-character text kernel) is a BASIC machine that runs as an ELF game on the PlusCart and the UnoCart. VCSIDE builds this program into its ROM: at power-on the program is entered as if typed, then run.

## Kernel rules
- TinyELF Basic draws the screen itself: its ARM code feeds the TIA every frame and runs the BASIC program in between. The program never writes the TIA; `POKE` reaches TinyELF's own memory.
- The interpreter runs a budget of tokens between two frames. A tight loop of `POKE`s can need a lower one: the sketch pad sets `POKE 653,0: POKE 652,100` so that the TV keeps its sync.

## The program
- `src/main.lst` is a plain TinyELF listing (the format of TinyELF's `.lst` examples): every line starts with its line number, keywords in capitals. It is the text that `LIST` shows and `ENTER` reads; it is not tokenized or checked when it is built, so a syntax error shows up when the line runs, on the 2600's screen.
- The screen: 36 columns of text (`PRINT`, `LOCATE`, `CLS`); `GRAPHICS` switches to a playfield bitmap with `PLOT`, `DRAWTO` and a text window below it.
- Input: `GET K` reads a key from a KeyPortari keyboard (0: none; cursor keys up 11, down 10, left 8, right 21, RETURN 13, ESC 27); `INPUT` reads a line. `STICK(0)` and `STRIG(0)` read the joystick (bits 0-3 clear: up, down, left, right; `STRIG(0)=0`: fire). A held key also shows in `STICK`, as left and right at once.
- `DELAY ms` waits; `POKE` reaches TinyELF's own memory (sound registers, colours, the frame budget): see TinyELF's manual.
- The end of the program (`END`, the last line, an error, or ESC) leaves TinyELF at `READY`, with the program still in memory; the sound is switched off then.

## Build
- F7 compiles TinyELF Basic's sources (VCSIDE's `data/tinyelf`, GPL-3) with the program in it into `build/{{NAME}}.bin`. It needs the Arm GNU Toolchain with its C library (newlib); Homebrew's `arm-none-eabi-gcc` has none.
- F5 runs it in Gopher2600 with `-keyportari ASCII` (Project > Emulator options): the computer's keyboard is the KeyPortari. Stella cannot run it (Cortex-M4 code).
- Memory: the program takes about as much BASIC memory as its text (48 KB in Gopher2600, 64 KB on the carts). The ROM's limit is the loader's 96 KB, of which TinyELF takes about 85.

## Do not touch
- Nothing is generated: every file is the user's.
