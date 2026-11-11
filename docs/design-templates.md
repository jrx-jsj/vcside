# Project templates: one set for every language and cartridge

Status: proposal (2026-11-11), for review before any code. What exists today is in [design.md](design.md) (each language's template) and [design-ace.md](design-ace.md#the-ace-template-datatemplatesace-cc0-phase-3); how templates share files (`data/templates/shared`, `shared.json`) is in [design.md](design.md).

## Goal

Every language or cartridge type offers the same kinds of starting point in *New project → Start from*, so that a newcomer can compare them and move from one to another:

1. **Empty project**: only what is needed to start writing and to build: the includes, the build file, and a frame loop that keeps the screen stable (VSYNC, blank lines, overscan; on ARM cartridges, the overblank and an empty kernel). Nothing to delete before starting.
2. **Basic game**: the *same small game* in every type (below): a player character, enemies, a shot, a playfield, music (played by the TIA on cartridges without an ARM, sampled on ARM ones) and sound effects, with a **title screen and a menu**.
3. **Showcase** (optional per type): something that shows what that choice adds.
4. **Banked title variant** (only for formats larger than 2K and 4K): the basic game with its title screen and menu in another bank, to show how bankswitching is used.

The examples that exist today become showcases or are replaced by the basic game (table below), so no current example is lost without a replacement.

## The basic game

One design, written once per language, sharing its graphics and its songs (`data/templates/shared`). Small enough to fit a 4K DASM cartridge with the TIA music player.

- **Title screen**: the game's name (playfield letters, or a 48-pixel text where the kernel allows it) and a **menu** moved with the joystick: up and down choose a row, left and right change its value, the button acts on the row.
  - **PLAY**: the first row and the one selected at start. The game starts only by choosing it (the button on PLAY), because "press the button" alone leaves people lost.
  - **SYSTEM: NTSC / PAL60 / PAL**: the TV system, as in La Abadía del Crimen 2600 (`la-abadia-del-crimen/docs/pal-ntsc.md`): NTSC 262 lines, PAL60 262 lines with PAL colours, PAL 312 lines. It changes the frame's lines and the colour table at run time. Preselected from what the cartridge says where it says something (ELF and ACE below); otherwise from the project's TV standard.
  - **HMOVE: NORMAL / SHORT** (ELF only, if it fits): preselected from `FF_REDUCED_HMOVE`; the consoles with the short HMOVE are explained in the Abadía document above (ZackAttack's measurements, Eckhard Stolberg's note).
  - **EXIT** (only when the cartridge has a menu to go back to: `FF_MULTI_CART` on ELF, always on ACE): back to the PlusCart's or UnoCart's menu, as RESET does in today's templates.
  - Texts in English, like the rest of the templates.
- **Playfield**: a room with walls and a few blocks, which stop the character and the shot.
- **Player**: `hero.vcsgfx` (today's `idle` and `walk` animations), moved in four directions and mirrored when it turns.
- **Enemies**: two or three that walk and bounce off the walls (one sprite, flickered or repositioned on different bands, depending on the kernel), in a new `enemy.vcsgfx`.
- **Shot**: a missile in the direction the character faces; hitting an enemy removes it and scores.
- **Score and lives**: a score at the top or bottom; touching an enemy costs a life; after the last one, back to the title screen.
- **Sound**: the theme on the title screen and during play; effects for the shot, a hit and a lost life. TIA cartridges use `shared/theme-tia.vcsmusic` (two channels, effects that take a channel); ARM cartridges use `shared/theme.vcsmusic` (sampled voices on AUDV0, one TIA effect on channel 1), as now.

## Per type

| Type | Empty | Basic game | Showcase | Banked title |
|---|---|---|---|---|
| DASM, 2K/4K | today's `dasm` template, trimmed to the loop | 4K, TIA music | today's animated sprite (`dasm-animation`) | — |
| DASM, F8/F6/F4 | 8K skeleton with the bank switch code | — (it is the banked variant) | — | the basic game in bank 0, the title screen in bank 1 (F8; the Makefile/size notes say how to grow to F6 and F4) |
| batari Basic | `main.bas` with the loop only | standard kernel, TIA music | (proposal: the DPC+ kernel) | `set romsize 8k`, the title in `bank 2` |
| DASM, DPC+ | driver and the loop | sampled music (DPC+'s three voices) | today's DPC+ music example | the title in another 4K bank |
| DASM, CDFJ | Makefile, `main.asm`, `game.c` with the loop | sampled music, logic in C | today's CDFJ example (C and music) | the title in another bank |
| C, ELF | Makefile, vcsLib, `elf_main` and an empty kernel, showing every argument | the game | today's rainbow and bar (or bus stuffing) | — (ELF has no banks) |
| C, ACE | Makefile, `ace.h`, an empty kernel | the game on `ace.h` | what the M4 adds: speed, FPU, 96 KB of RAM | — |
| C, ACE (vcsLib) | — | the ELF game, built with `make ace` (as the ACE example "the ELF game on vcsLib" today) | — | — |

## ELF: `elf_main`'s arguments, shown and used

The empty project and the game both read and comment every argument, with what each cartridge passes (verified for the Abadía, `la-abadia-del-crimen/docs/pal-ntsc.md`, 2026-10-30):

- `args[MP_SYSTEM_TYPE]`: 0, 1 or 2 (NTSC, PAL, PAL60) from Stella's `Display.Format`, the PlusCart's TV menu option, Gopher2600's TV; measured by the Chameleon. Trusted only when it is 1 or 2 (0 may be a cartridge that does not fill it); it preselects the SYSTEM row.
- `args[MP_CLOCK_HZ]`: the ARM's clock in Hz in Stella, Gopher2600 and the PlusCart, but timer ticks on the Chameleon. The template shows why it must not be used to tell the system or to time anything.
- `args[MP_FEATURE_FLAGS]`, read with masks (the Chameleon passes 6): `FF_MULTI_CART` shows EXIT and lets RESET leave; `FF_CHAMELEON_CART` means the 6507 is already in an overblank (`vcsEndOverblank` first); `FF_REDUCED_HMOVE` preselects HMOVE: SHORT.
- What happens if `elf_main` returns, per cartridge (back to the menu on the PlusCart; a fatal error in Stella; a loop on the Chameleon): the template only returns when `FF_MULTI_CART` is set.

ACE has no such arguments: the ACE templates take the system from the project's TV standard (as `{{VISIBLE_LINES}}` does today) and the clock from `ACE_ARGUMENTS->clock_hz`, and keep the same menu.

## What is shared

- **Graphics**: `hero.vcsgfx`, `enemy.vcsgfx`, the title's letters: one copy each in `shared/`, copied by `shared.json`.
- **Songs**: the two that exist (TIA and sampled), with each template's effects as a `change` in its `shared.json`, as now.
- **C game logic**: the rules (movement, collisions with the playfield, enemies, shot, score) in a plain C file with no bus calls, shared by ELF, ACE and CDFJ; each type keeps its own kernel, which is what differs. ELF and ACE-on-vcsLib share the whole game.
- **DASM**: `2600.h`, the TIA music player, and the title/menu code shared by the 4K, banked, DPC+ and CDFJ games where the kernel allows it.

Every created project still gets plain files (CC0) and no reference back to VCSIDE.

## Checks

Each new template is created and built by `vcside_tests` (NTSC, PAL and, where it applies, AUTO), and checked with Gopher2600 macros (frame count and stability in each SYSTEM choice, a screenshot of the title, the menu and the game, the joystick and the button, a WAV recording for the music and the effects); the ELF ones also in Stella. The banked variants: the bank switch checked in the debugger.

## Phases

1. This document, reviewed.
2. The shared graphics (`enemy.vcsgfx`, title letters) and the C game logic.
3. DASM 4K: empty, game; then the F8 banked variant.
4. ELF: empty (arguments), game; ACE: empty, game on `ace.h`, showcase.
5. batari Basic: empty, game, banked variant.
6. DPC+ and CDFJ: empty, game, banked variant.
7. Documentation, *New project* texts and translations.

## Open questions

- Menu texts in English only, or bilingual like Mists of Aldar (English by default, a LANGUAGE row)?
- Two or three enemies? Three need flicker or bands in the 4K kernel.
- The banked DASM variant: F8 only (with notes for F6/F4), or a choice of size?
- batari Basic's showcase: the DPC+ kernel, or none?
- ELF's showcase: keep today's rainbow, or something that shows the ARM's speed (bus stuffing, many sprites)?

## Credits

The menu with the SYSTEM row and the reading of `elf_main`'s arguments come from La Abadía del Crimen 2600 (this repository's sister project). The short HMOVE: ZackAttack's measurements and Eckhard Stolberg's note (AtariAge, *HMOVE variations*, 2018). These go into [sources.md](sources.md) with the code that uses them.
