# VCSIDE

A small native IDE for making Atari 2600 games, on macOS, Windows and Linux. It is a single executable built with C++17, Dear ImGui and SDL3, and is not based on VS Code.

> Status: first version (0.1). Design and phases in [`docs/design.md`](docs/design.md).

## What it does today

- **Projects**: `.vcsproj` files (JSON) with language, main file, ROM, cartridge format, TV standard, emulator and illegal-opcode policy. New projects come from a template for DASM, batari Basic or C for ELF, or from an example (*Start from*): for DASM and for C, an animated sprite drawn in the graphics editor, walking with the joystick.
- **Build** with DASM (F7). Errors and unresolved symbols go to *Problems*, and clicking one opens the line. ELF projects use their own `build_command` (e.g. `make`). Any project can build with a command of its own (`build_command`: `make`, `sh build.sh`, or a line with `&&` run by the shell), use a DASM or batari Basic of its own instead of the one in *Settings → Tools* (`tool_paths`) and pass more arguments to DASM (`build_flags`, e.g. `-DPAL=1`). The project's settings are in *Build → Project settings...* (or clicking the `.vcsproj` in the tree): a form, or the JSON as text with a switch between both; keys VCSIDE does not use are kept. Opening a folder that has a Makefile or a build script and no project offers to create one for it, with the main source and the ROM read from the folder.
- **ELF cartridges** (C on the cartridge's ARM, with arm-none-eabi-gcc): the new-project template has a Makefile, its own `vcsLib.h`, the long calls to the loader and the checks of the ELF rules. `make` gives the ROM (`build/<name>.bin`, a relocatable object for Stella, Gopher2600, PlusCart and UnoCart); `make chameleon CHAMELEON_FW=<folder>` links it with the Chameleon Cart firmware (its template is not included). gcc and ld errors, undefined symbols with the file that uses them and make's own errors go to *Problems*.
- **batari Basic** (1.9, the official WebAssembly build, with [wasmtime](https://wasmtime.dev/)): F7 runs its `2600basic.sh`/`2600bas.bat` next to the `.bas`. Its own errors and DASM's errors in the generated assembly go to *Problems* on the line of the `.bas` (through the `;;line N;;` marks of the `.asm`). The ROM, listing and symbols are left as `main.bas.bin`, `.lst` and `.sym`, so breakpoints, *Go to address* and the emulators' symbols work as in DASM. The space report takes the bytes left per bank from its output. The editor colours its keywords, the registers and the labels, with hover help and completion; F12 goes to a label.
- **Run** in Gopher2600 (the default for new projects) or Stella (F5 build and run, F6 debug). The cartridge format and TV standard are passed on the command line, and both emulators load the `.sym` symbols of the build.
- **Breakpoints**: click in the left margin or press F9. F6 places each one on its instruction's address and bank (from the listing, following `RORG`) and passes them to the emulator:
  - Gopher2600 gets them all in a script (`-script`);
  - Stella gets them all in `build/<rom>.script`, which its debugger runs when it opens (VCSIDE points its user folder at `build/`).
  - They are kept in `<project>.vcsproj.user` (add it to `.gitignore` if you don't want to share it).
- **Emulator options** (*Build → Emulator options...*): how each emulator starts, per project. Sound, debug colours, the TV switches and difficulty switches, controllers, palette and TV effects, random RAM/CPU/TIA on reset, breaks on bad port accesses, TIA revisions, ARM checks for DPC+/CDFJ/ELF... (54 Stella options, 21 Gopher2600 ones, with their help on hover), plus free extra arguments. TV standards: NTSC, PAL, PAL60, SECAM, NTSC50 and SECAM60 (Stella), PAL-M (Gopher2600), or *Multiple (in the code)* for a program that handles several systems itself (the DASM template then chooses with `-DPAL=1`); a new project offers the ones its language can build for.
  - The emulators start with a private copy of your own settings (Stella: `-basedir`, or `HOME` for the macOS app; Gopher2600: portable mode), because Stella saves every command-line option when it quits. So these options never end up in your settings, and changes made inside an emulator started from VCSIDE are not kept.
- **Go to address** (Ctrl+Shift+G): from an address the emulator shows (PC, a break) to its source line, in every bank where it exists.
- **Tools** are found in `PATH` and the usual install folders, or set by hand in *Settings → Tools*. Before building or running, VCSIDE checks what the project needs; if something is missing, a window says what it is and how to install it, with its official download page, *Choose the file...* and *Search again*, and then carries on:
  - DASM projects: DASM;
  - batari Basic: its `2600basic.sh` / `2600bas.bat` (which brings its own DASM) and, for 1.9, wasmtime;
  - ELF: the Arm GNU toolchain and GNU Make (their folders are put first in `make`'s `PATH`, since an app opened from the desktop does not have the shell's);
  - running: the project's emulator.
- **Editor**:
  - DASM syntax colouring, with illegal opcodes in their own colours (purple for stable ones, red for unstable ones).
  - A cycle column for every instruction (e.g. `4/5` when crossing a page, `2/4` for a branch). Symbols from the last build are used to tell zero page from absolute.
  - Select lines to see their total cycles, bytes and fraction of a scanline in the status bar.
  - Hover help for instructions, registers and symbols, plus completion (Ctrl+Space; Control+Space on macOS, where Cmd+Space is Spotlight). Labels, constants and macros count as soon as they are written, in the open files and in the project's other sources, before any build; constants show their value and the cycle counts use it. A symbol of the last build that its sources no longer define (deleted or renamed) is no longer offered.
  - Ctrl/Cmd+click on a file a line names opens it: DASM's `include` and `incbin`, C's `#include`, batari Basic's `include`, `inline` and `incgraphic`, and the `.vcsgfx` of a `rem vcsgfx` block. It is looked for next to the file, in the project, in its include folders and in `include/`.
  - Indentation as the file has it: Tab, Shift+Tab, Backspace and Enter go by its levels, of a tab or of the spaces most of its lines use. In C, Enter after `{` or a braceless `if (...)` goes one level in, and adds the `}` when the block is not closed yet; a `{` or `}` typed on a line of its own goes where its block says; Enter in a block comment goes on with ` * `.
  - Folding (arrows in the gutter, Ctrl/Cmd+Alt+Up and Down, *Edit → Fold all*): regions found from the text. DASM: `MAC`/`ENDM`, `IF`/`ENDIF`, `REPEAT`/`REPEND`, `SEG` and `SUBROUTINE` sections, each global label to the next one and runs of comments. batari Basic: `function`, `data`, `asm`, blocks such as `player0:` up to `end`, the `rem vcsgfx` blocks, labels and runs of `rem`. C: braces, `#if`/`#endif` and comments.
- **Documentation panel** (F1 on a word):
  - all 256 6502 opcodes, grouped as legal, stable illegal and unstable illegal;
  - TIA and RIOT registers with their known issues;
  - cartridge formats and TV standards.
- **Space report** after each DASM or batari Basic build:
  - bytes used and free per ROM bank, in the 2600's RAM (with room left for the stack) and in the cartridge's RAM (Superchip, FA, CV);
  - warnings for full banks, wrong ROM sizes and code on bank-switch hotspots, with the source line when possible;
  - thresholds per project: `space_warn_percent` and `stack_reserve` in the `.vcsproj`;
  - ELF projects: code, code in RAM (`.RamFunc`), constants and variables read from the ROM's sections, and the Chameleon Cart's flash (128 KB) and RAM (36 KB). Exact when the Chameleon image is built, otherwise estimated with the firmware's measured share.
- **Graphics editor** (`.vcsgfx`, *File → New graphics...*):
  - Sprites built from the TIA's five movable objects (P0, P1, M0, M1, BL) as layers, each with its position, width, reflection and colour per row and frame, in animation frames. A multicolour sprite is any mix of them.
  - Flicker phases: layers in different phases are drawn on alternate frames, so an object can be used more than once; the preview shows the flicker.
  - Playfields of 40 columns whose rows each have their own mode (mirror or repeat), colours and, if drawn, their own right half (asymmetric): changes in the middle of the screen and of the line.
  - Kernel profiles: the graphics know what draws them, a kernel of one's own or one of batari Basic's (standard with its `kernel_options` and `pfres`, multisprite, DPC+, PXE), taken from the project's main program unless the file chooses another. The editor offers only the objects the kernel draws (P0-P16 where it has them), hides the row colours it cannot change, hatches the playfield columns it does not show and mirrors multisprite rows.
  - Drawn as the TIA and the kernel would (object priority, a missile in its player's colour, one colour where the kernel has one), with warnings for that kernel.
  - Named animations: steps that show a frame for a number of video frames, looping or not, played in the preview. DASM gets frame pointer tables and per-animation step tables (frame, duration); batari Basic gets a subroutine to `gosub` once per frame, which can also be kept up to date in the program.
  - batari Basic programs keep the graphics up to date: the lines between `rem vcsgfx <file>.vcsgfx <name> [frame N | animation NAME]` and `rem vcsgfx end` are rewritten with the current blocks every time the graphics are saved (*Copy as → batari Basic, kept up to date* gives them ready to paste).
  - The tables can go to one file, one file per sprite and playfield (`<name>.<item>.gfx.asm`) or one per group (`<name>.<group>.gfx.asm`), chosen in the editor's toolbar; files an earlier choice generated are removed.
  - ELF projects get `<name>.gfx.h` on save: `const uint8_t` arrays per frame, `#define`s, the animations as step arrays and `vcsgfx_animate()` to play them from C.
  - Projects with a kernel of their own get `<name>.gfx.asm` on save; *Copy as...* gives DASM tables or the kernel's batari Basic blocks (row order, `playerNcolor:`, 32- or 16-column playfields, `DFnFRACINC` for DPC+ and PXE). *Import* reads those, PlayerPal's assembly tables, batari Basic's blocks and the playfields of masswerk's Tiny VCS Playfield Editor (with its mode and line height) from the clipboard.
- **Illegal-opcode policy** per project: allow, stable only, warn or forbid.
- **Menus** in the system menu bar on macOS (application menu with About, Tools and Language; Window menu), and in the window on Windows and Linux. Ctrl shortcuts are Cmd on macOS; Replace is Cmd+Shift+F there, since Cmd+H hides the app, and completion is Control+Space, since Cmd+Space is Spotlight.
- **About** (*VCSIDE → About* on macOS, *Help → About* elsewhere): the system's About panel on macOS, a window of its own on Windows and Linux. Both show the icon, the version, VCSIDE's licences, the libraries built in, the tools it works with by group (compilers and assemblers, emulators, build helpers) with the versions it was tested with and their licences, the sources of the knowledge base, the Solarized palette and the fonts.
- **Themes** (*View → Theme*): Dark, Light, Solarized Light and Solarized Dark, plus your own. Every colour of the IDE is a named role (interface, dialogs, code editor, syntax, messages, space report, graphics editor), and the code editor can shade each indentation level. *Edit themes...* changes them live: a theme can build on another one and keep only its differences, use a palette of named colours, refer to other roles and set alpha. Themes are `.vcstheme` files (JSON) that can be exported, imported, copied as text and pasted.
- **Several windows**, one project each: *File → New window* (Ctrl+Shift+N, Cmd on macOS) opens one on the welcome page; *Close project* takes a window back to it; closing a window (Ctrl+Shift+W) removes it, and the last one goes back to the welcome page before the program quits. A project already open in another window brings that window forward. Settings (theme, zoom, recent projects) are shared.
- **Welcome page** when nothing is open: the icon, *New project*, *Open project*, *Open folder* (any folder, with or without a project) and the recent projects and folders, one click away.
- **Images**: PNG and BMP files open in a viewer with sharp pixels. A click zooms in (Alt+click or the right button zooms out), the wheel zooms at the pointer, dragging moves the image; the bottom line gives the pixel and its colour. The view follows the file when it changes on disk.
- **Zoom** of the whole IDE: Ctrl+`+`/`-`/`0` (Cmd on macOS), Ctrl + mouse wheel in the editor, or *View*. The fonts are drawn again at each size, so they stay sharp; the zoom is remembered. Shortcuts go by the character of the key, so they work on any keyboard layout (Ctrl+`/` is Ctrl+Shift+7 on a Spanish one).
- **Interface languages**: English, plus catalogs in `data/i18n/` (Spanish included). Catalogs can be partial, and anything missing falls back to English.

## Building

You need CMake 3.24+, a C++17 compiler and, optionally, SDL3 installed (otherwise it is downloaded and built).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

- macOS: `build/VCSIDE.app`
- Windows / Linux: `build/vcside[.exe]`, with `data/` next to it.

Linux and Windows builds can also be made from any machine with Docker (`tools/docker/`):

```sh
docker build -t vcside-linux -f tools/docker/linux.Dockerfile tools/docker
docker run --rm -v "$PWD":/src -w /src vcside-linux sh tools/docker/build-linux.sh      # build-linux/, runs the tests
docker build -t vcside-windows -f tools/docker/windows.Dockerfile tools/docker
docker run --rm -v "$PWD":/src -w /src vcside-windows sh tools/docker/build-windows.sh  # build-windows/vcside.exe
```

The Windows one is cross-compiled with MinGW-w64 and linked statically (SDL3 included): `vcside.exe` and `data/` are the whole program. Its tests run in Wine with `VCSIDE_TEST_DATA=Z:<repository>/data wine64 build-windows/vcside_tests.exe`. `VCSIDE_CONFIG_DIR=<folder>` makes VCSIDE keep its settings there instead of the user's (for tests). VCSIDE needs OpenGL 2.1 or later; it tries 3.0, 3.2 core and 2.1, and says so in a message box if none works.

## Releases

`tools/release.sh X.Y.Z` publishes a version from a Mac with Docker and `gh`: it builds and tests macOS (universal), Linux and Windows, tags `vX.Y.Z`, creates the GitHub release with the packages, checksums and notes, and updates the download page of the website (`vcside-web`). What is new in each version goes in `docs/releases/X.Y.Z.md`. The whole process, and why the builds are not signed yet, is in [`docs/releases/README.md`](docs/releases/README.md).

## Translations

```sh
python3 tools/i18n_check.py                 # what is missing in each catalog
python3 tools/i18n_check.py --update        # add new keys (empty = English)
python3 tools/i18n_check.py --new fr Français
```

UI strings go through `T("...")`, or `F("... {0} ...", {...})` when they have values. Translations may reorder the placeholders.

## Knowledge base

`data/kb/*.json`: `6502.json` is generated by `tools/gen_6502.py`, and the TIA, RIOT, cartridge-format and TV data is written by hand. The sources for each fact are listed in [`docs/sources.md`](docs/sources.md).

## Icon

`assets/icon/vcside.svg` is the source (plus `vcside-16.svg` and `vcside-32.svg`, pixel versions for the smallest sizes). `python3 tools/make_icons.py` rebuilds the PNGs, `vcside.ico`, `vcside.icns` and `data/vcside.png` (needs `rsvg-convert`; `iconutil` on macOS). It is an original drawing, with no trademarks.

## Licences

- Code and icon (`assets/icon/`): **GPL-3.0-or-later** ([`LICENSE`](LICENSE)).
- Knowledge base (`data/kb/`): **CC BY-SA 4.0**.
- Project templates (`data/templates/`): **CC0** (public domain), so the games you make are yours to license as you like.
- Themes (`data/themes/`): **CC0**, except the Solarized palette (MIT, Ethan Schoonover).

Third-party licences are in [`licenses/`](licenses/README.md). External tools (DASM, Stella, Gopher2600…) are not bundled.
