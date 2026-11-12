# VCSIDE — design

A minimalist, native IDE for making Atari 2600 games (7800 support with the Chameleon Cart later on). It is not derived from VS Code or any web editor: a single executable that works the same on **macOS, Windows and Linux**.

## Decisions

| Topic | Decision | Reason |
|---|---|---|
| Language | C++17 | Builds with clang, gcc and MSVC; the same code on all three systems. |
| Interface | Dear ImGui (*docking* branch) + SDL3 + OpenGL 3.2 | Lightweight, no system dependencies, dockable panels, easy to draw graphic editors (sprites, playfield). |
| Processes | SDL3's `SDL_Process` | Launch assemblers and emulators and read their output without per-system code. |
| Text editor | ImGuiColorTextEdit (adapted) | Own highlighting for DASM, batariBasic and C; hooks for autocompletion and block selection. |
| Menus | Described as data (`src/menu.*`): ImGui draws them on Windows and Linux; on macOS they go to the system menu bar (`src/platform/menu_macos.mm`) | On macOS every application uses the system menu bar. The application still handles the shortcuts (the menu only displays them), so each shortcut lives in a single place; those of the application and Window menus (Cmd+Q, Cmd+H, Cmd+M…) are the system's. |
| Data | JSON (nlohmann/json) | Projects (`*.vcsproj`), settings and knowledge base are readable and versionable. |
| Build | CMake ≥ 3.24 + FetchContent | System SDL3 if present; otherwise it is downloaded. ImGui, the editor and JSON are always downloaded at a pinned version. |
| Language (i18n) | English as the source language; catalogues `data/i18n/<code>.json` with the English text as key | Adding a language means adding a file; catalogues can be partial (anything missing appears in English); `F()` with `{0}`, `{1}`… lets phrases be reordered; `tools/i18n_check.py` lists what is missing. The knowledge base is written in English (later, `data/kb/<code>/` layers with the same fallback). |
| Illegal opcodes | Kept separate in the data (`illegal`, `stable`) and in the interface (colours, groups, warnings) | Each project chooses: allow, stable only, warn or forbid. |
| Sources and licences | Every knowledge-base datum cites its source (`docs/sources.md`); nothing is copied verbatim | Dependencies (zlib/MIT) ship their licence next to the executable; GPL tools are not redistributed. |

## Modules

```
src/
  main.cpp          SDL3 window + ImGui loop
  app.*             global state, menus, docked panels
  project.*         workspace and project (.vcsproj): files, toolchain, format, TV
  toolchain.*       tool detection (PATH, typical paths per system, user settings)
  builder.*         runs the build and parses errors (file:line → jump to the editor)
  emulator.*        launches Stella / Gopher2600 with the project's ROM, format and TV
  kb.*              knowledge base: opcodes, registers, formats, TV standards
  cycles.*          cycle counter for a selection of assembly
  editor/*          code tabs, highlighting, autocompletion, hover help
  tools/*           sprite, playfield and sound editors (built in or external)
data/kb/            6502.json, tia.json, riot.json, formats.json, tv.json
```

## Windows and Linux

- **Building:** in Docker (`tools/docker/`), from any system. Linux: Debian trixie with its SDL3. Windows: MinGW-w64 from Debian, statically linked against SDL3 3.4.18 built separately; the `.exe` needs no DLLs.
- **Testing:** the `vcside_tests` tests pass on all three. The Linux one is viewed with Xvfb and Mesa (`xvfb-run`); the Windows one, in Wine 6 on macOS, which only offers OpenGL 3.2 core or 2.1 (hence the attempts 3.0 → 3.2 core → 2.1).
- **`process.h`:** the name clashes with the Windows header that MinGW's threads include; that is why it is `child_process.h`.
- **Processes on Windows:** SDL quotes every argument when calling `CreateProcess`, and `cmd /c` does not understand that. For `cmd /c ...` (the project's own build commands and batari Basic's `2600bas.bat`) VCSIDE writes the full line, `cmd.exe /c ""C:\a b\x.bat" "main.bas""`, with `SDL_PROP_PROCESS_CREATE_CMDLINE_STRING` (SDL 3.4).
- **Not tested on a real Windows** nor with an emulator there: only in Wine.

## Tools

Each tool is looked up in this order: path pinned in settings → `PATH` → typical system paths (e.g. `/Applications/Stella.app`, `C:\Program Files\Stella`, `/usr/games`). If it is not found, the IDE offers the official download page and the folder where to put it (`~/.vcside/tools/<tool>`); later on, direct download of the published releases with their checksum.

The Tools window and About list them in three groups (`Toolchain::groups()`): compilers and assemblers, emulators, and build helpers (wasmtime for batari Basic, GNU Make for ELF). cc65 was dropped: no template or build uses it, and 2600 programs are written for DASM.

| Tool | Use | Notes |
|---|---|---|
| DASM | 6502 assembler | `dasm main.asm -f3 -omain.bin -lmain.lst -smain.sym`; errors `file (line): error:` |
| batari Basic 1.9 | BASIC → DASM | official WebAssembly version: `2600basic.sh/.bat` and wasmtime; ships its own DASM |
| ELF-VCS (arm-none-eabi-gcc + vcsLib) | C for ELF cartridges | template with the Makefile from the Chameleon/PlusCart ELF rules |
| Tiny-ELF PXE Basic | BASIC for ELF | — |
| Stella | run and debug | `stella -format <type> -tv.* ... rom.bin`; `.sym` symbols and `.lst` listing next to the ROM |
| Gopher2600 | run and debug | `gopher2600` binary; ELF with the *nightly* 07d8de5 or later |

## Knowledge base

- **6502**: the 151 official opcodes and the stable illegal ones, with modes, bytes, cycles, +1 on page crossing / branch taken, flags and description.
- **TIA and RIOT**: each register with address, read/write, bits, usage, timing (e.g. `RESP0` in the colour cycle) and known issues (HMOVE at cycle 73, `RESBL` in HBLANK, collisions, differences between TIAs).
- **Formats**: 2K, 4K, F8, F6, F4, FE, E0, E7, 3F, 3E, FA, DPC, DPC+, CDFJ/CDFJ+, ELF, ACE; with *hotspots*, extra RAM, detection and the name Stella and Gopher2600 expect.
- **TV**: NTSC, PAL, PAL60, SECAM: lines, frame rate, palettes and tips.

Autocompletion, hover help and the documentation panel are generated from these JSON files.

## Cycle counter

When a block of assembly is selected, the minimum and maximum cycles (page crossing, taken branches), the bytes and the position within the line (76 cycles per line, 228 colour clocks) are added up. It is shown in the status bar and as a column next to the code.

## Emulators and debugging

Checked with Stella 7.0 and Gopher2600 v0.56.1, by reading their source code and by trying them out.

- **Symbols:** both automatically load the `.sym` that sits next to the ROM with the same name. Stella also loads the `.lst`.
- **Breakpoints:** set in the margin or with F9 and saved in `<project>.vcsproj.user`.
  - With F6, each one is moved to the first instruction of its line or the following ones.
  - The address and bank come from the DASM listing: the listing gives the `ORG` address, so `RORG`/`REND` is followed in each segment. Lines of a macro are assigned to the line that calls it.
- **Gopher2600:** `DEBUG -script <file>` runs the script when the debugger starts, with lines such as `BREAK PC $F100 & BANK 1`. It saves nothing in its configuration.
- **Stella:**
  - **It saves everything:** command-line options are saved to its configuration on exit (`Settings::load`/`save`); only the local ones (`-basedir`, `-break`, `-debug`, `-holdreset`, `-holdselect`) and the ROM properties (`-bs`, `-format`, `-tv`, `-ld`, `-lc`…) are spared.
  - **Own copy:** that is why VCSIDE starts it with a copy of the user's configuration, renewed at every launch, in `<VCSIDE config>/emulators/`. In the Unix and Windows versions with `-basedir`; in Stella.app, which ignores `-basedir` (it is removed with `#if 0`), with `HOME` pointing to the copy, because it looks for `~/Library/Application Support/Stella` through `HOME`.
  - **Breakpoints:** the `<rom>.script` it runs when the debugger opens is looked up in its user folder (`userdir`, `Debugger::autoExec`), not next to the ROM as the manual says. With the own copy it is given `-userdir build/` and VCSIDE writes `build/<rom>.script` there (`break $F100 1`, all banks).
  - **Snapshots:** `-snapsavedir build/` unless the project says otherwise.
- **Launch options:** `data/kb/emulators.json` (CC BY-SA, like the rest of the knowledge base) describes each option: group, type, values, Stella argument (`dev: true` if it needs `-dev.settings 1`, which VCSIDE adds by itself), Gopher2600 option or preferences key, and translations. The project stores only the ones that are changed (`"emulator_options"` and `"emulator_args"` in the `.vcsproj`); the rest is left as the emulator has it.
- **Gopher2600 and preferences:** sound, VSYNC checking, random state, TIA quirks… are only read from the `preferences` file (the command-line preferences are only for the comparison emulation). If any is changed, it starts in portable mode: a working folder with `portable.txt` and `Gopher2600_UserData/` with a copy of the user's files and those changed keys.
- **TV standards:** NTSC, PAL, PAL60 and SECAM in both; NTSC50 and SECAM60 only in Stella; PAL-M only in Gopher2600. If the emulator does not have the chosen one, it is not passed and the emulator picks (with a warning). Each language offers the ones it can generate (`tvStandardsFor`): in DASM all of them, because the kernel is the programmer's and the template sets the 50 or 60 Hz lines; in batari Basic NTSC, PAL (the template adds `set tv pal`, its only other option) and PAL60 (its NTSC rate with PAL colours); in C for ELF, NTSC, PAL and PAL60, which vcsLib detects at startup. A project that has another one set by hand keeps it in the menus. There is also "Multiple (in the code)" (`AUTO` in the `.vcsproj`): the program works on more than one system and resolves it itself. The DASM template then puts the lines for both rates behind `IFCONST PAL`, so that `-DPAL=1` (*DASM arguments*) gives the 50 Hz version; batari Basic chooses with `set tv` and C detects it at startup. No standard is passed to the emulator: it deduces it from the frame. The graphics editor then shows the NTSC palette.
- **Go to address** (Ctrl+Shift+G): from an address (or label) and, optionally, a bank, to its line. It takes the 6507 mirrors into account ($F000 = $1000).
- **Pending:** receiving from the emulator the address where it stopped (they have no channel to notify another application).
- **batari Basic:** the listing is that of the generated `.asm`; each line is mapped to the one in the `.bas` by the last `;;line N;;` mark that precedes it (what comes before the first one and after the program belongs to no line).
- **ACE:** only Gopher2600 runs ACE files (`-mapping ACE`); choosing the ACE format switches the project to it, and Stella is marked "no ACE" in the *Emulator* menu. See [design-ace.md](design-ace.md).
- **ROMs in the tree:** a double click on a ROM in the *Project* panel runs it with the project's emulator and options; the format comes from its header (ACE), from the project for its own ROM, or AUTO.
- **ELF:** breakpoints are not passed on (neither Stella nor Gopher2600 can stop at a C line); `-bs ELF` and `-mapping ELF`.

## ELF projects

- **Template** (`data/templates/elf`, CC0): `Makefile`, `include/vcsLib.h`, `src/main.c`, `src/memcpy_long.S` and `chameleon/support.c`, written for VCSIDE following the ELF rules of the games in this folder (`reglas-elf.md`): Cortex-M0, `-fdata-sections` without `-ffunction-sections`, partial link with libgcc, `strip --strip-unneeded`, libgcc divisions renamed, long calls to the loader, `memcpy`/`memset` trampolines and `RAM_FUNC` for the kernel. The example is a rainbow with a bar that moves with the joystick; it returns to the menu with RESET in a multicart.
- **Checks after linking:** each undefined symbol that does not belong to the loader comes out as `src/<file>.c: error: undefined symbol '...'` (with `-r`, the linker does not report it as an error, and Stella would link it to 0); and no short call to the loader.
- **ACE:** `make ace` builds the same game as an ACE file for the PlusCart and the UnoCart, with vcsLib written on the ACE template's bus library (`ace/` in the project). See [design-ace.md](design-ace.md#elf-games-as-ace-files-make-ace-phase-5).
- **Chameleon Cart:** `make chameleon CHAMELEON_FW=<chameleon_cart folder of its template>`. Zack's template is not included because it has no licence.
- **Errors:** besides `file:line:`, messages without a line (the checks, `arm-none-eabi-gcc: error:`), those from ld (`main.c:(.text+0x1c): undefined reference`, with the function; overflowed region) and the make ones that do not repeat another error. Files that ld gives without a folder are looked up in the project.
- **Space:** VCSIDE reads the ELF directly. From the ROM (relocatable): code, `.RamFunc`, constants, initialised and zeroed variables. If there is a `build/<rom>-chameleon.elf` newer than the ROM, its program headers give the exact flash (what is stored from `0x0800_0000`) and RAM (what lives at `0x2000_0000`); otherwise it is estimated from what the template's firmware of 2025-10-23 takes up, measured: about 10,100 bytes of flash and 6,000 of RAM (the Abadía estimate ends up 41 bytes from the real one). STM32G070RB limits: 128 KB and 36 KB.

## ACE projects

- **Template** (`data/templates/ace`, CC0) and the `ace-animation` example: C for the PlusCart's and UnoCart's Cortex-M4, run in Gopher2600 at 168 MHz. The program runs from SRAM (copied by the start code after the console reboots into the cartridge), with its own bus library and overblank program; the C music player is the ELF one. Design and measurements in [design-ace.md](design-ace.md#the-ace-template-datatemplatesace-cc0-phase-3). Its second example, *the ELF game on vcsLib*, is the ELF template as it is with `make ace` as the build command: the same source as an ELF project, which still builds the ELF ROM with plain `make` (it shows that an ACE cartridge runs vcsLib games; the `ace.h` templates show what ACE adds).
- **Debug:** `build/ACE_debugging.elf` (`-gdwarf-4 -gstrict-dwarf`) gives Gopher2600's source-level debugger; VCSIDE does not pass breakpoints.
- **Space:** from `build/ACE_debugging.elf`: the file against the firmware's 896 KB, what runs in SRAM against the 96 KB buffer, and a warning when Gopher2600 gives the file less SRAM than the program needs (64,000 bytes from 448,000 bytes of file up).

## batari Basic projects

- **Compiler:** batari Basic 1.9 in WebAssembly (GPL-2), run with wasmtime. It is looked up in *Settings → Tools* or at `<VCSIDE config>/tools/bb/2600basic.sh`. The script runs in the `.bas` folder (wasmtime only lets it see `.`), with the `bB` variable pointing to its folder and wasmtime's folder in front of the `PATH`, because an application opened from the Finder does not have the terminal's `PATH`.
- **Success:** the script always ends with 0, even when it fails. Previous outputs are deleted before building; the build is good if there are no errors and `main.bas.bin` appears.
- **Outputs:** `main.bas.bin`, `.asm`, `.list.txt` and `.symbol.txt` are left next to the `.bas`. VCSIDE copies the ROM to the project's output and the listing and symbols to `<rom>.lst` and `.sym`, the names that Stella and VCSIDE look for.
- **Errors:** its own (`line 4: Error: ...`) already carry the `.bas` line; the DASM ones in `main.bas.asm (L)` are mapped to it with the `;;line N;;` marks. A jump to a label that does not exist (`Unknown Mnemonic 'jmp .x'`) is explained as such.
- **Space:** from its output, `N bytes of ROM space left [in bank K]`, with 4 KB banks (numbered from 0, as in the emulators). RAM is not measured: its variables are fixed.
- **Format:** the cartridge chosen in *New project* (4K, F8, F6 or F4: `set romsize 4k`, `8k`, `16k` or `32k` in the template); a folder found without a project is `AUTO`, because the size and the kernel are set by the program (`set romsize`, `set kernel`).
- **Editor:** `data/kb/batari-basic.json` (CC BY-SA) with the compiler's words (names taken from `keywords.c` and `statements.c`) and own summaries in English and Spanish. Words with `N` and a `range` are expanded (`joyNup` → `joy0up` ... `joy3up`). Labels are whatever starts in column 0.
- **Template** (`data/templates/bb/main.bas`, CC0): a square that moves with the joystick inside a playfield with walls. From 8k on, batari Basic puts its kernel in the last bank and the program in bank 1 (`VCSIDE.md` says how to add banks); 2k is not offered, because the template and its music player do not fit.

## Graphics editor

A `.vcsgfx` file (JSON, with the rows written as `X` and `.` so that it reads and diffs like a drawing) stores sprites and playfields. It opens in an editor tab (*File → New graphics...*).

- **Sprites as object layers.** A sprite is composed from the TIA's five movable objects (P0, P1, M0, M1, BL), each on a layer with its position in colour clocks and rows, its width (NUSIZ for players; 1, 2, 4 or 8 for missiles and ball), its reflection and its colours per row and frame. This way a multicolour character can be, for example, P0 + P1 overlapped and a missile as a sword.
- **Flicker phases.** Each layer has a phase (1 to 4): those in the same phase are drawn in the same frames. An object can be repeated in different phases, never twice in the same one. The preview alternates the phases at 60 Hz or shows them all at once.
- **What the hardware does.** The canvas and the preview draw like the TIA: P0/M0 priority over P1/M1 over ball and playfield, and each missile with the colour its player has on that line. The proportions approximate those of a TV (a colour clock is wider than tall on a line).
- **Playfields by rows.** 40 columns; each row has its mode (mirrored or repeated, bit 0 of CTRLPF), its playfield and background colours and, if it is drawn in its right half, its own right half (asymmetric). This describes changes in mid-screen (of mode, colour, drawing) and mid-line.
- **Kernel profiles.** Each `.vcsgfx` knows what will draw it: a custom assembly kernel (the whole TIA) or a batari Basic one (standard, multisprite, DPC+ or PXE). By default it is the project's: in assembly, the custom one; in batari Basic, the one set by the main program (`set kernel`), with its `set kernel_options` and `const pfres`, which are re-read when the file changes. The file can pin another (`kernel` field). The editor adapts to what the kernel displays:

  | | Custom | Standard | Multisprite | DPC+ | PXE |
  |---|---|---|---|---|---|
  | Players | P0, P1 | P0, P1 | P0-P5 | P0-P9 | P0-P16 |
  | Missiles and ball | shaped | blocks; `player1colors`/`playercolors` remove M1 (and M0), `no_blank_lines`/`readpaddle` remove M0 | one-line missiles | blocks | blocks |
  | Colour per row | all | P1 with `player1colors`, P0 with `playercolors` | no (one `COLUPn` per player) | players | players |
  | Row order | top to bottom | bottom to top | bottom to top | top to bottom | top to bottom |
  | Playfield | 40 columns, mode per row | 32 columns from the 5th, asymmetric; 11 rows or `pfres` | 16 columns from the 5th, mirrored | 32 columns | 32 columns |
  | Playfield colour per row | playfield and background | with `pfcolors` (background with `background`) | no | `pfcolors:` and `bkcolors:` | `pfcolors:` and `bkcolors:` |

  Players that the kernel does not have are not offered (with the reason shown on mouse-over), per-row colours disappear where the kernel gives a single one, the columns it does not show are hatched and cannot be painted, and in multisprite each row is drawn mirrored. The canvas and the preview show what the kernel will display. The exports were checked by compiling them with batari Basic 1.9 on the four kernels and viewing them in Stella.
- **Kernel-dependent warnings.** They warn of an object used twice in a phase or that the kernel does not draw, of flicker and its frequency, of extra players that share lines (the kernel alternates them), of the shared colour of missiles and ball, of the writes each line will need in a custom kernel, of asymmetric rows and their timing margin, and of what a batari Basic kernel does not show (columns, missile and ball shape, per-row colours without the option that enables them, number of rows).
- **Output.** In DASM projects, saving writes `<name>.gfx.asm` with labelled tables (`<sprite>_<object>[_<layer>]_Frame<n>`, `_Colour<n>`, positions and sizes as constants; playfields in per-register tables, with PF0R/PF1R/PF2R for the right half and CTRLPF and colour tables if they change). Rows run from bottom to top, as most kernels read them. *Copy as...* gives the same text or the kernel's batari Basic blocks (`player0:`…`player16:`, `playerNcolor:`, `playfield:` of 32 or 16 columns, `pfcolors:`, `bkcolors:`), with the rows in the order it reads, fixed colours as assignments (`COLUP0`, `_COLUP1`, `COLUP2`…) that go before each `drawscreen`, and in DPC+ and PXE the `DFnFRACINC` values that give the rows their height. In an assembly project it is copied for the standard kernel with the options the graphics need.
- **Output files.** The tables go to a single file (default), to one per sprite or playfield (`<name>.<element>.gfx.asm`) or to one per group (`<name>.<group>.gfx.asm`; each element has a `group` field, and those without one stay in `<name>.gfx.asm`). It is chosen in the editor bar (`split` field of the `.vcsgfx`: `each` or `group`). On saving, the files that were previously generated from that `.vcsgfx` and no longer apply are deleted, recognised by their first line; the others are left alone. In C, the same with `.gfx.h`, each with its include guard.
- **Animations.** A sprite has named animations: steps that each show a frame for a number of video frames, looping or stopping on the last one. The same frame can appear in several steps (walking: 0-1-2-1) and with different durations. When frames are inserted, deleted or moved, the steps follow them. The preview plays the chosen animation. In assembly, tables of pointers to the frames of each layer are produced (`_FramePtrLo/Hi`, `_ColourPtrLo/Hi`) and, per animation, `<sprite>_<animation>_FRAME` and `_TICKS` (frame and duration of each step) with `_STEPS` and `_LOOP`: the program keeps a counter and a step index and reads the frame from there. In batari Basic a subroutine is produced (`gosub <sprite>_<animation>` once per frame, `_reset` to start over) with the durations in a `data` table, an `on ... goto` to the step's frame and the blocks of each frame; it uses two variables chosen in the animation. It can also be kept up to date in the program with `rem vcsgfx <file> <sprite> animation <name>`. It was checked by compiling it with batari Basic 1.9 on the standard and DPC+ kernels and watching in Stella that it changes frame at its own pace.
- **Cartridge in New project** (DASM and batari Basic; ELF has its own row): the formats with a template that builds, `Project::cartridgeFormats`. DASM: 2K and 4K (the empty template at `$F800` or `$F000`), F8, F6 and F4 (`data/templates/dasm-banked`, CC0: 2, 4 or 8 banks of 4K; every bank ends with the same code at `$FFE0`, the vectors switch to bank 0 whatever bank the cartridge starts in, and bank 0 calls `FrameLogic` in bank 1 through a trampoline at the same address in both banks), DPC+ and CDFJ (their examples only). *Start from* lists only what is made for the chosen cartridge: the game and the animated sprite are 4K. The project's format is the chosen one; the project settings can change it later, but not the code. Each choice was built and run in Stella (NTSC and PAL).
- **Assembly example** (*New project → Start from → Example: an animated sprite*, `data/templates/dasm-animation`, CC0): a character drawn in `hero.vcsgfx` (from `data/templates/shared`, see below) with two animations, `idle` (blinks) and `walk`. When the project is created, `hero.gfx.asm` is written as the editor writes it on saving. The program draws it with a two-lines-per-row kernel that reads the frame through a pointer (`hero_P0_FramePtrLo/Hi`, `_ColourPtrLo/Hi`) and, during vertical blank, advances the animation with its `_FRAME`/`_TICKS`/`_STEPS`/`_LOOP` tables. The joystick moves and mirrors it. It assembles in NTSC and PAL (automatic test) and the blinking and the walking step were checked in Stella.
- **Assembly game** (*New project → Start from → Example: a small game with a title menu*, `data/templates/dasm-game`, CC0; the first DASM example): the basic game of [design-templates.md](design-templates.md) on a 4K cartridge. A title screen with the cartridge of VCSIDE's icon (an asymmetric playfield with a colour per row, the label's sides drawn by the missiles), "THE GAME" as 48-pixel text and a menu (PLAY; TV: NTSC / PAL60 / PAL, which changes the frame's lines and the colours at run time, every colour through a nearest-colour NTSC→PAL table, the cartridge's rows included; the first one comes from the project's TV standard, `{{TV_SYSTEM}}`). The game: the hero of `hero.vcsgfx`, two enemies of `enemy.vcsgfx` that take turns on player 1 (flicker), a shot on missile 0, the room of `room.vcsgfx`, the hardware collisions, the score and lives in score mode, back to the title after the last life; the TIA song with shot, hit and lose effects. All five files come from `data/templates/shared` (the song with its effects as a `change`). Checked in Gopher2600 with macros in the three systems (262, 262 and 312 lines, stable on both screens), the shot, the score, the lost lives and the return to the title; the techniques and their authors are in [sources.md](sources.md).
- **C output (ELF projects).** In an ELF project the kernel is also custom, but in C: saving writes `<name>.gfx.h` next to the `.vcsgfx`, with an include guard. Each layer gives `static const uint8_t <sprite>_<layer>_gfx[<SPRITE>_FRAMES][height]` (with the drawing and the row's colour in a comment) and `_colour`, with the rows from top to bottom, which is how a line loop walks them; position, height, phase, NUSIZ and REFP go in `#define`s. Each animation is an array of `{frame, duration}` steps and a `vcsgfx_animation` with its number of steps and whether it restarts. The types and two `static inline` functions, `vcsgfx_play` (switches animation if it is not the one playing) and `vcsgfx_animate` (once per video frame, returns the frame due), appear only once even if several `.gfx.h` files are included. Playfields give per-register arrays (`_pf0`…, `_pf0r`… if there are asymmetric rows, `_ctrlpf`, `_colupf`, `_colubk` if they change). *Copy as → C arrays* gives the same for one element. A test compiles the header of several graphics included twice with `-Wall -Wextra -Werror`.
- **C example** (*New project → C for ELF (vcsLib) → Start from → Example: an animated sprite*, `data/templates/elf-animation`, CC0): the ELF template with a different `main.c` and the same `hero.vcsgfx` in `src/` (one copy, in `data/templates/shared`); when the project is created, `src/hero.gfx.h` is written. The kernel positions player 0 with RESP0 after 10 + x/6 nops (it ends up 1-6 pixels to the right; below 10 it would fall in the horizontal blank) and HMOVE puts it back in place; then it writes GRP0 and COLUP0 on every line. During overblank the joystick moves it in the four directions and picks `walk` or `idle`. It builds with `make` without warnings (automatic test if arm-none-eabi-gcc is present) and the animation and the exact position from 0 to 159 were checked in Stella.

- **Shared template files** (`data/templates/shared`): the repository keeps one copy of what several templates use. Each template folder that needs one has a `shared.json` that maps a path in the new project to a file in `data/templates`, with an optional `change` merged over JSON files (RFC 7386, written as the editors save them). `shared/theme.vcsmusic` is the sample-played song of the ELF, ACE, CDFJ and DPC+ templates (CDFJ and DPC+ change only its export target); `shared/theme-tia.vcsmusic` is the TIA song of batari Basic and the assembly sprite example (which renames an effect, its export target and the song); `shared/hero.vcsgfx` is the sprite of the three animation examples and the game; `shared/enemy.vcsgfx`, `shared/room.vcsgfx` and `shared/title.vcsgfx` are the game's enemy, room and title letters. The ELF template's `shared.json` also lists the ACE template's bus library, start code, `memcpy`/`memset` and linker script that go into `ace/`. `Project::create` copies them like the template's own files, so a created project holds plain files and no reference back to VCSIDE.
- **Project notes for the assistant** (`VCSIDE.md`): every template folder has one, copied into the new project with its name (the example's, or else the base template's; C templates are copied whole). Short and in English, it says only what is true in that template: the kernel rules that must not be broken, where the joystick, the button, the drawing and the sound live, how it builds, and the files not to edit (generated tables, drivers, the loader's interface). The assistant reads it into its first message ([design-assistant.md](design-assistant.md)); `vcside_tests` checks that each template gives it, under the 4000 characters the assistant uses.
- **Graphics kept up to date in a batari Basic program.** batari Basic only includes assembly, not its own code, so the blocks have to be in the `.bas`. The lines between `rem vcsgfx <file>.vcsgfx <name> [frame N | animation NAME]` and `rem vcsgfx end` belong to VCSIDE: every time the `.vcsgfx` is saved, they are rewritten in the project's `.bas` files with the export for its kernel (one frame, if indicated, so each can be put in its branch of the animation). If the program is open, it is changed in the editor (undoable) and saved, unless it had changes of its own; otherwise, on disk. *Copy as → batari Basic, kept up to date* gives the zones ready to paste, one per frame. A zone with a name that does not exist, an extra frame or one without `end` is left as it was and a warning is given in the output.
- **Interchange.** *Import* reads from the clipboard those same tables, PlayerPal's (`.byte #%...;$xx`, with the row's colour in the comment) and batari Basic blocks (in the kernel's row order; if the text is a program with `set kernel`, its own), and the playfields of masswerk.at's *Tiny VCS Playfield Editor*: its header `; mode: symmetric|asymmetric repeat|mirrored line-height N` gives the rows' mode and their height, and both a `.byte` line per row (in the first column, with three or six values) and a table per register are accepted, which in asymmetric mode alternates the left and right half of each row, so graphics can go back and forth between VCSIDE and those tools. There are links to PlayerPal and Atari Background Builder for those who prefer to draw there.
- **Colours.** The NTSC and PAL palettes are computed with each standard's arrangement of hues and luminances (they are approximate; the emulator shows the exact ones). SECAM uses its eight fixed colours by luminance.

## Self-building projects

Some projects already come with their own way of building (a Makefile, a `build.sh`, a `make.bat`). When their folder is opened without a `.vcsproj`, the *Project* panel says so and offers to create one (`Project::detect`): the command (`make` or the script), the language and the main source (`main.*`, otherwise the first `.asm` with `processor`, the `.bas` or the `.c`) and the ROM, which is the first word of the Makefile or script ending in `.bin`, `.a26` or `.elf` that is not a variable or a pattern. The command runs in the project folder with the folders of the tools found placed in front of the `PATH`; if it contains `&&`, `|`, `;` or redirections, the system shell runs it (`/bin/sh -c` or `cmd /c`). The DASM and GCC errors it prints go to *Problems*.

The `.vcsproj` is edited in a tab (*Build → Project settings...*, or with a click in the tree if it is the open project) as a form or as text, with a selector to switch between them: when switching to text, what the form holds is written, and when going back the text is read (if it is not valid JSON, it stays on the text with the error). On saving, the open project takes the changes. Keys that VCSIDE does not use are preserved (`Project::otherKeys`), and if the menus change a setting, a tab with no changes of its own is updated.

Each project can use its own DASM or batari Basic (`tool_paths`, paths relative to the project or absolute) instead of the one in *Settings → Tools*, for example to pin a version, and pass extra arguments to DASM (`build_flags`); those that change the code (`-D`, `-M`, `-I`) also go to the pass that measures the space.

## Code folding

`src/editor/folding.cpp` extracts the regions from the text, without compiling anything: in DASM, `MAC`/`ENDM`, `IF`/`IFCONST`/`IFNCONST` up to `ENDIF`/`EIF`, `REPEAT`/`REPEND`, each `SEG` up to the next one, each `SUBROUTINE` and each global label in column 0 up to the next one (or up to the next `SEG`), and runs of two or more comments. In batari Basic, `function`, `data`, `asm` and blocks such as `player0:` or `playfield:` up to `end`, the `rem vcsgfx` blocks, labels and runs of `rem`. In C, braces spanning several lines (a brace alone on its line is joined to the one above), `#if`/`#endif` and comments. Sections do not keep the blank lines or the comments that close them, which usually introduce what follows. If two regions start on the same line, the larger one is kept.

The editor shows rows instead of lines: a folded region leaves its first line with a mark saying how many it hides. Folds follow the text like breakpoints, and open if the cursor enters them (a search, *Go to line*, an error). The up and down arrows skip what is folded.

## Project tree

The *Project* panel draws the tree row by row: arrow, icon and name, with vertical guides per level and the whole row highlighted on mouse-over or if it is the file being edited. Folders come first; `build` and what it contains appear dimmed. The icons are own vector shapes (`src/file_icons.cpp`) that take the theme's colour: a sheet with a letter for code (A assembly, C, H headers and includes, B batari Basic, L listings and symbols), a sprite for `.vcsgfx`, a cartridge for ROMs (`.bin`, `.a26`, `.elf`, `.ace`), a joystick for the `.vcsproj`, a frame for images and a ruled sheet for the rest.

## Themes

Every colour in VCSIDE has a named role, grouped as: `ui.*` (interface), `editor.*` (code editor), `syntax.*` (syntax colours), `messages.*` (errors, warnings, notes), `space.*` (space report), `graphics.*` (graphics editor), `dialog.*` (dialogs) and `files.*` (project tree: rows, guides and one colour per file type). The list is in `src/theme.h`; the code never uses a fixed colour, except for the 2600's own (the NTSC/PAL/SECAM palette of the graphics editor). All the ImGui colours are computed from the `ui.*` roles, and a theme can correct any of them by name in `imgui`.

A theme is a `.vcstheme` file (JSON):

- `base`: the theme it takes whatever it does not specify from. A theme based on another stores only its differences and follows the base's changes.
- `palette`: named colours. A role can be `#RRGGBB`, `#RRGGBBAA`, `$name` (from the palette), `@role` (another role) and any of them with `/AA` to fix the alpha (`$blue/40`). References are resolved after merging the bases, so changing a palette entry or `ui.accent` changes everything that refers to them.
- `colours`: the roles. `indent`: one colour per indentation level in the editor (empty list, no shading), with the `editor.indentGuide` line where each level starts. `imgui`: individual ImGui colours.

Four come built in (`data/themes`): Dark (the colours as they have always been), Light, Solarized Light and Solarized Dark. Both Solarized themes use Solarized's published palette with names by function (`bg`, `bgHi`, `fg`, `fgEm`, `dim`); the dark one is the light one with those five entries swapped, as the palette itself does. A problem (a role without a colour, a reference that does not exist, a loop of references or of bases) is shown in magenta and listed in the theme editor.

*View → Theme* switches theme. *Edit themes...* opens a window with the themes (built-in and yours), the palette, the roles by group with a search box, the indentation levels and the problems. Changes are seen immediately and saved automatically in the user's themes folder (`<settings folder>/themes`). Built-in themes are not changed: "New, based on this one" creates an empty one that follows it and "Duplicate", a copy. To share: export to a file or copy as text, and import a file or paste a theme. What is exported carries inside the bases that are not VCSIDE's, so it only needs a built-in theme. When importing, a theme is never overwritten: if the name exists, another one is given.

Dialogs (new project, tools, unsaved changes...) have their own look, that of the website and the cartridge box: a dark title band with three stripes below it (yellow, orange and red), a thick border, more room around, labels on the left and the primary button in orange, which turns yellow under the mouse. The colours come from the `dialog.*` roles: the built-in themes use all those of the website (ink, yellow, orange and red) and change only the background, and a custom theme can change them. Every panel and tool window carries the same three stripes as a hallmark, beside its title (`theme::drawStripes()`): docked, in the tab row after the tabs; floating, in a dark title band like a dialog's, between the title and the close button (`theme::beginPanel()` and `theme::endPanel()` instead of `ImGui::Begin()` and `End()`). The other buttons of a dialog are dark and turn red under the mouse, and headings inside a dialog are a dark band with an orange line, like the boxes of the website. In code: `theme::beginModal()` instead of `ImGui::BeginPopupModal()`, `theme::primaryButton()` for the primary button, `theme::dialogButton()` for the rest and `theme::dialogHeading()` for headings.

## Space warnings

After every build with DASM, the IDE measures how much space is left (`src/space.*`). Each format's data (valid ROM sizes, bank size, reserved addresses, extra RAM and its windows) is in the `space` object of `formats.json`.

- **How it is measured:**
  - **ROM:** a second DASM pass with `-f2` writes each block of emitted bytes with its origin and length, so it is known exactly what is occupied. `ALIGN` and `ds` padding counts as occupied. The listing is no use for this: `incbin` gives no size and macros carry stale addresses.
  - **RAM:** the uninitialised (`U`) lines of the listing, with each `ds`'s size read from its code line and evaluated with the symbols. Variables declared with `=` are not counted; if there is no `SEG.U`, RAM comes out as "not measured".
- **ROM:**
  - what is used and what is free per bank;
  - error if the image is larger than the format allows, warning if it is of a different size;
  - warning if code or data fall on a *hotspot* or on the cartridge RAM's ports. It does not warn about `ds`/`align` padding, which is the normal way to skip over the Superchip window.
- **2600 RAM:** what the variables take up and what is left above them for the stack. Warning if less is left than the project's reserve, and error if a variable goes past `$FF`.
- **Cartridge RAM:**
  - Superchip (128 bytes), FA (256) and CV (1 KB): declarations through the read or the write window are counted together;
  - E7 comes out as "not measured", because its RAM is addressed by banks.
- **Thresholds:** warning on reaching `space_warn_percent` (90 % by default) and error on overflow. The stack reserve is `stack_reserve` (8 bytes by default). Both are configured in the `.vcsproj`.
- **Where it is shown:** the *Space* panel (one bar per bank and for RAM), the summary in the status bar and the warnings in *Problems*, with their line when possible.
- **Pending:**
  - formats without space data (3E, DPC, DPC+, CDFJ…), which are measured as 4 KB banks;
  - warning about reads of the Superchip's write port.

## Pending ideas

- **Music editor** (0.2.0): TIA music with its tuning shown and solved, and sample synthesis for ELF and the ARM cartridges. Proposal in [design-music.md](design-music.md).
- **Optional AI assistant**: an agentic chat panel that works on the project's files and VCSIDE's tools, with small local models or, if the user chooses, remote APIs; off by default. Proposal in [design-assistant.md](design-assistant.md).
- **Tutor mode** for the assistant (idea from the user, 2026-11-10): a guided path for someone who wants to make a game, adapted to their level, recommending readings for what they want to achieve. Notes in [design-assistant.md](design-assistant.md#tutor-mode-idea-for-later).
- **Cancel a build in progress** (a button and a menu command), and retry if a save coincides with it.
- **Send the ROM to the PlusCart** — done 2026-11-11, not tried on a cart: *Build > Serve to the PlusCart (Wi-Fi)* makes VCSIDE the cart's API host on the local network. See [design-ace.md](design-ace.md#sending-the-rom-to-the-pluscart-phase-6). Still open: copying to the UnoCart's SD card.
- **ACE cartridges** (UnoCart, PlusCart): proposal and proof of concept in [design-ace.md](design-ace.md).
- **Image for the Chameleon Cart**: generate the file that is written to the cartridge from the ROM, choosing the firmware to use.
- **Find references** of a symbol (every place where it is used) and **view its definition in a floating window** without leaving the file.
- **File outline**: the list of labels, subroutines, macros and constants, to jump to any of them.

## Phases

1. Skeleton: window, workspace, editor, build with DASM, run in Stella. 6502/TIA/RIOT knowledge base, cycle counter.
2. Autocompletion, hover help, documentation panel; full editor (find/replace, go to definition…); ROM and RAM space warnings; batariBasic; Gopher2600.
3. ELF-VCS and Tiny-ELF; project templates per format.
4. Sprite, playfield and sound editors (and export/import with community editors: PlayerPal, Atari Background Builder, TIATracker…).
5. Tool download assistant; packaging for the three systems.
6. 7800 with the Chameleon Cart.
