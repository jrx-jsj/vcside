# ACE cartridges — design proposal

Status: proposal (2026-11-10). Phases 1 (knowledge and running ACE files), 2 (the proof of concept, `tools/ace_poc`), 3 (the template) and 4 (the space report) are done, except for real hardware. Sources are at the end and in [sources.md](sources.md#ace-cartridges-read-2026-11-10).

## What ACE is

ACE ("ARM Custom Executable") is a program for the STM32F407 (Cortex-M4F) of the **UnoCart 2600** and the **PlusCart**. ZackAttack (Zack Scolaro) added it to the UnoCart firmware in June 2018, first to update the firmware from the SD card. Marco Johannes adapted it for the PlusCart and the unified firmware of both carts (United Carts of Atari, UCA) in 2021.

The firmware copies the file and jumps into it. From then on, the game's ARM code *is* the cartridge:

- it watches the 6507's address bus on one GPIO port;
- it drives the data bus through another, every bus cycle.

There is no bankswitching scheme, no driver and no fixed layout: whatever the 6507 reads is what the ARM chooses to answer. The UCA firmware itself ships its DPC+, CDFJ and ELF emulators as ACE-wrapped programs.

| | DPC+ / CDFJ | ELF (vcsLib) | ACE |
|---|---|---|---|
| Who drives the bus | a driver in the cart's ARM; the 6507 runs a ROM | the loader's vcsLib, from a queue the game fills | the game's own ARM code |
| Program form | 6507 ROM + ARM blob in banks | relocatable ELF object, linked by the loader | flat binary with a 40-byte header |
| CPU | ARM7TDMI (Harmony/Melody) | Cortex-M0+ (Chameleon) or M4 | Cortex-M4F only |
| Carts | Harmony, Melody, Encore, UnoCart, PlusCart | PlusCart, UnoCart, Chameleon | UnoCart, PlusCart (and ports of the loader: PlusCart-Pico, FujiNet 2600) |
| Stella 7.0 | yes | yes | **no** |
| Gopher2600 | yes | yes | yes, since v0.20.0 (2023-02-11) |

## The file

The header is a packed struct of 40 bytes, little-endian. Its fields are those of `ACEFileHeader` in ZackAttack's `ace2600.h` (Strong-ARM-for-VCS). The offsets were also checked in Gopher2600 and in the three Wolf3D ACE files in `busstuffing/roms`.

| Offset | Size | Field | Wolf3D |
|---|---|---|---|
| 0 | 8 | magic: `ACE-2600` (original UnoCart), `ACE-UF00` (UCA, any cart), `ACE-PC00` (UCA, PlusCart only), `ACE-UC00` (UCA, UnoCart only) | `ACE-UF00` |
| 8 | 16 | driver name, padded with spaces (emulators use it, e.g. `elf-relocatable`) | `WOLF3D Demo` |
| 24 | 4 | driver version | 0 |
| 28 | 4 | ROM size to copy | `0x0007DFFF` |
| 32 | 4 | checksum (UCA reads it and never checks it) | 0 |
| 36 | 4 | entry point | `0x3041` |
| 40 | … | the program | |

**Entry point.** Its meaning depends on the magic:

- `ACE-2600`: an absolute address. The file is linked as a complete firmware at `0x08010000` and is called with no arguments.
- `ACE-PC00`: an absolute address, assuming the file sits at `0x08020200`.
- `ACE-UF00`: an offset from the start of the file, wherever the firmware put it. Both carts copy it to a flash download area whose address depends on what else is stored (see below). **So UF00 code must be position-independent.**

The firmware calls the entry with `((int (*)())entry)()` and does not set the Thumb bit itself, so the header's value must have bit 0 set (Wolf3D's code starts at `0x3040`).

**Size.** UCA rejects files bigger than 896 KB; the original UnoCart takes up to 448 KB.

**Wrapped formats.** Gopher2600 detects two kinds when its *unwrap ACE* preference is on (the default) and runs them with its own mappers:

- an ACE with `elf-relocatable` in its first 144 bytes carries an ELF object, whose offset is the 16-bit value at `0x28` (id `ELF_in_ACE`);
- one with `DPCp` carries a DPC+ game.

## What the program receives

The UCA firmware leaves twelve 32-bit words at the start of SRAM, `0x20000000`. Gopher2600 builds the same block.

| Word | Meaning |
|---|---|
| 0 | address of the ACE file |
| 1 | CCM RAM (`0x10000000`, 64 KB) |
| 2 | the firmware's `reboot_into_cartridge()` |
| 3 | `emulate_firmware_cartridge()`: back to the menu |
| 4 | `SystemCoreClock` in Hz |
| 5 | ACE interface version (UCA `0x00010000`; Gopher2600 2) |
| 6 | firmware version (Gopher2600: PlusCart revision 3) |
| 7 | address-bus input register (`ADDR_IDR`) |
| 8 | data-bus input register (`DATA_IDR`) |
| 9 | data-bus output register (`DATA_ODR`) |
| 10 | data-bus mode register (`DATA_MODER`) |
| 11 | end mark `0xACE42600` |

The registers differ between the two carts, but the firmware hides it (`cartridge_io.c` in UCA):

- **PlusCart:** address on GPIOD (`0x40020C10`); data on the low byte of GPIOC (`0x40020810`, `0x40020814`, mode `0x40020800`).
- **UnoCart:** address on GPIOD; data on bits 8-15 of GPIOE. The pointers it passes already point at the high byte of `GPIOE->IDR` and `->ODR` and the high half of `->MODER`.

So a program that writes a byte through word 9 and `0x5555` (16 bits) through word 10, as the firmware's own macros do, runs on both carts unchanged. There is no official map of the RAM a program may use: the CDFJ driver overwrites `0x20000000` from offset `0x1C` onwards and uses CCM.

**Where the file goes.** The PlusCart (from its online storage or offline flash) and the UnoCart (an `ACE-UF00` from the SD card) copy the file to a flash download area: `DOWNLOAD_AREA_START_ADDRESS` + 128 KB × (first free sector − 5), or the offline flash address + 512. Only `ACE-UC00` runs from the RAM buffer. `ACE-PC00` adjusts its absolute entry by `file address − 0x08020200`.

## How the program drives the bus

There is no clock line. The core loop, in our words:

1. read the address register;
2. if A12 is set (a cartridge address), put the byte on the data output and switch the pins to output;
3. otherwise release the bus. If wanted, read the data input to snoop a TIA or RIOT write.

There are roughly 140 ARM cycles at 168 MHz for every 6507 cycle (1.19 MHz), so a game either runs its logic between bus cycles or keeps the 6507 busy (NOPs, overblank) while it computes.

ZackAttack's Strong-ARM framework (`vcsLib.c`) is the origin of the vcsLib names: `InjectRomByte`, `YieldDataBus`, `SnoopDataBus`, `vcsWrite3`, `vcsLda2`, `vcsSta3`, `vcsJmp3`, `StartOverblank`… The ELF loaders later took the same API. **An ACE template can carry its own vcsLib-compatible bus library**, so the same C game builds for both ELF and ACE.

**Gopher2600** emulates the bus differently from the real cart:

- it lets the ARM run until it has read the address register six times per 6507 access, then takes `ODR` as the cartridge's answer (`ace.go`, `AccessPassive`);
- the firmware callbacks are a stub that only returns;
- its ARM clock is the `hardware.arm7.clock` preference: 70 MHz by default, the Harmony's. An ACE project should set 168.

## Proof of concept (`tools/ace_poc`, CC0)

`make` builds `build/test.ace` (4,192 bytes). It is an `ACE-UF00` file whose ARM loop (a dozen instructions) serves a 4 KB DASM ROM that draws colour bars.

- **Linking.** It is linked at `0x08020000`, where Gopher2600 puts ACE files. The code reaches its data as an offset from `__ace_start` plus word 0 of the argument block, so it runs wherever the firmware copies it. The linker script computes the header's size and entry (with bit 0 set); the assembler cannot subtract symbols from other files.
- **Both carts.** It uses the firmware's register pointers as they are: 8-bit data, 16-bit mode. That matches what UCA passes on both carts.

Results in Gopher2600 `07d8de5`:

| Run | Result |
|---|---|
| `bars.bin`, the 4 KB ROM on its own, `PERFORMANCE -duration 3s` | 1,253 frames |
| `test.ace`, the same | 332-342 frames (about 3.7 times slower to emulate) |
| the same file with the entry's bit 0 cleared | 342 frames: Gopher2600 accepts it either way (the firmware does not) |
| `RUN -macro` with `SCREENSHOT` | the colour bars, as the DASM ROM draws them |
| `HEADLESS`, `COPROC LIST SOURCEFILES` / `FUNCTIONS` | `main.c` and `ace_entry` at `0x08020028`-`0x0802005F`, from `build/ACE_debugging.elf` |

**Source-level debugging.** Gopher2600's developer tools look for an ELF with DWARF next to the ROM:

- the names `ACE_debugging.elf`, `armcode.elf`, `main.elf`, `custom2.elf` or `cdfj+.elf`;
- in the ROM's folder or in `main/`, `main/bin/`, `custom/bin/` or `arm/` (`coprocessor/developer/dwarf/elf_shim.go`).

Two conditions found by trying:

- **DWARF version 4 only.** GCC 16 writes version 5 unless given `-gdwarf-4`; with 5, the debugger silently finds no source files.
- **No relocation.** The ELF's addresses are used as they are (`ExecutableOrigin` exists but nothing calls it). The debug ELF must therefore be linked at `0x08020000`.

**Wolf3D** (`ACE-UF00`, 512,908 bytes) produces no frames in Gopher2600. Its debugger shows why: the emulator's limits, not the format.

1. Wolf3D writes `FLASH->ACR` (`0x40023C00`, the STM32's flash wait states), which Gopher2600 does not map.
2. Gopher2600 sizes the emulated SRAM by the file's size: 128,000 bytes below 64,000 bytes of file, 256,000 and 512,000 for bigger ones, and only 64,000 from 448,000 bytes up. The 2 KB stack sits right after it. Wolf3D clears memory past `0x20010200`, overwrites its stack and then runs off to illegal addresses.

**Lessons for a template:** keep the program's SRAM use well inside what both Gopher2600 and the cart give, and don't touch other STM32 peripherals unless they are guarded.

Still to do: a real PlusCart or UnoCart.

## The ACE template (`data/templates/ace`, CC0, phase 3)

Language **C for ACE** (`ace-c`), offered in *New project* next to C for ELF, with the same example (the animated sprite). It builds with its Makefile, like ELF; the project gets `format: "ACE"`, `build/<name>.ace`, Gopher2600 and its ARM clock at 168 MHz (the `armClock` option, Gopher2600's `hardware.arm7.clock`).

```
data/templates/ace/
  Makefile         cortex-m4, hard float, -O2 -gdwarf-4 -gstrict-dwarf, no C library; checks the entry
  ace.ld           header and start code in the file (at 0x08020000); the program runs from SRAM
  include/ace.h    the argument block, TIA and RIOT names, the bus library's API
  src/start.S      the ACE-UF00 header and the start code
  src/ace.c        the bus library and the overblank program
  src/memory.c     memcpy and memset (the Arm toolchain may come without newlib)
  src/main.c       the rainbow, the joystick bar and the music, as in the ELF template
data/templates/ace-animation/src/   main.c and hero.vcsgfx: the animated sprite
```

**Position independence: the program runs from SRAM.** The firmware copies the file to flash at an address that depends on the cart and its free sectors, so code linked for one flash address cannot hold pointers to its own constants. The template links everything but the header and the start code at `0x20000100`, after the argument block, and the start code copies it there. C then works as usual (pointers to tables, switch tables, function pointers), with no GOT and no checks. The cost is SRAM: the code and constants count against the 96 KB buffer (a program larger than about 90 KB would need a different scheme; the space report warns before that).

**The start code** (`start.S`, position-independent assembly, the entry at offset 40):

1. calls `reboot_into_cartridge` (word 2 of the argument block) first: the firmware serves its own menu banks from the SRAM buffer while the console reboots, so copying into SRAM must come after it;
2. copies the image from the file (word 0, the file's address, plus the linker's `__image_offset`) to SRAM and clears `.bss`;
3. calls `main()`. If it returns non-zero, it calls `emulate_firmware_cartridge` (word 3: back to the cart's menu) and returns its result to the firmware.

The Makefile fails if the start code does not follow the 40-byte header, since the header's entry is an offset.

**The bus library** (`ace.c`, our own; names `ace_*`, not vcsLib's): the 6507 fetches one byte per cycle from increasing addresses, so the library keeps the address of the next fetch and waits for it. When the 6507 puts that address on the bus, the ARM drives the byte; on a TIA, RIOT or RAM address it lets go of the data bus. An address counts once it reads the same twice in a row (the firmware does the same).

- **Taking over the 6507** (`ace_start`): it may be anywhere after the reboot. Every cartridge read gets `$24`; `BIT $24` takes opcode, operand and a TIA read, so once the 6507 runs it, a cartridge read right after a non-cartridge one is an opcode fetch, and that one gets `JMP $1000`. The reset vector also leads to `$1000`.
- **Instructions:** `ace_lda`, `ace_sta`, `ace_write` (LDA #, STA), `ace_wsync`, `ace_nops`, `ace_jmp` (once a line, or by itself near `$1F00`).
- **Reading** (`ace_read`): `LDA` of the address, then `STA $1FF0`; during the write cycle the ARM samples the data bus until the address changes and keeps the sample before the last.
- **Leaving** (`ace_leave`): `JMP ($FFFC)`, so the 6507 reads the reset vector, which the firmware's menu answers once `main()` has returned non-zero. In Gopher2600 the callbacks are stubs, and the program starts again.

**The overblank program** (our own, 44 bytes at `$80`-`$AB` of the 6507's RAM): 63 lines with VSYNC on the first three. It plays a 4-bit sample per line to AUDV0 from `$E0`-`$FE` (low nibble, then high) and jumps back to `$1000`. The ARM is free for the game logic meanwhile. Its layout is the one the ELF loaders' program uses, so VCSIDE's C music player (`theme.music.h`: `music_level`, `music_nibbles`, `music_tia`, 63 overblank lines) is used unchanged; the kernel writes a sample at the start of each visible line and the 31 nibble bytes during the first lines.

**Checked in Gopher2600 `07d8de5`** (macros with `SCREENSHOT`, `FRAMEINFO`, joystick commands, `-wav`): the template draws the rainbow and moves the bar, 312 lines in PAL and 262 in NTSC, stable, VSYNC of 3 lines. RESET leaves and the program starts again. The recording has 16 sample levels all through. The example's hero walks to both edges with the frame stable. The debugger lists the C files, 31 functions and their local variables.

**DWARF:** Gopher2600 reads version 4 only and none of GCC's extensions. With `-O2`, GCC writes `DW_OP_GNU_entry_value` (`0xF3`) even with `-gdwarf-4`, and Gopher2600 then shows its "unsupported DWARF" notice; `-gstrict-dwarf` removes it.

- **Debug:** breakpoints in C are not passed on (VCSIDE says so when running); Gopher2600's own debugger shows the source from `build/ACE_debugging.elf`.
- **Space report (phase 4):** after each build VCSIDE reads `build/ACE_debugging.elf` (`analyzeAceSpace`). The file's header and start code are in flash; code, constants (`.rodata`, its own section in `ace.ld` so it can be told apart), variables with a value and zeroed variables are in SRAM. Three limits:
  - the **file** against 896 KB: the firmware rejects a header whose size is bigger (`get_ace_type`). On the PlusCart the ROMs stored offline take part of that flash, from sector 5 to the end of its 1 MB;
  - the **SRAM** the program reaches (from `0x20000000`, with the arguments) against the 96 KB download buffer (`BUFFER_SIZE` in `main.h`); the rest of the SRAM is the firmware's, with the stack;
  - the SRAM **Gopher2600** gives the file: 128,000 bytes below 64,000 bytes of file, 256,000 below 192,000, 512,000 below 448,000 and 64,000 from there up (`ace/memory.go`). Only a warning: the program may still run on the cart.
- **No cart choice.** For `ACE-UF00` the firmware does the same on both carts: the same size check, the same flash area and the same buffer, and the register pointers hide the different pins. A PlusCart / UnoCart setting would change nothing, so there is none.

## Phased plan

1. **Knowledge (small) — done 2026-11-10.**
   - `ACE` entry in `formats.json` (category `arm`, Stella `none`), with the header, the argument block and the differences between the carts.
   - Stella cannot run ACE (`formatSupported`). Choosing the ACE format switches the project to Gopher2600, the *Emulator* menu marks Stella "no ACE", and running with Stella anyway gives an error that says what to choose. Gopher2600 gets `-mapping ACE`.
   - `.ace` files are ROMs: cartridge icon, a Makefile that names one is detected as an ACE project (format ACE, Gopher2600), and the assistant treats them as binary.
   - A double click on any ROM in the project tree runs it with the project's emulator and options. The format comes from its header (`romFormat`: ACE), from the project for its own ROM, or AUTO for the rest. An ACE ROM goes to Gopher2600 even in a Stella project, with a note.
2. **Finish the proof of concept — done 2026-11-10, except real hardware.** The picture checked with a Gopher2600 macro. The debugger reads `ACE_debugging.elf` (DWARF 4, linked at `0x08020000`). Wolf3D's failure explained (Gopher2600's SRAM for big files and an unmapped flash register). The UnoCart's data layout handled by the firmware's pointers. Missing: running it on a real PlusCart or UnoCart.
3. **Template — done 2026-11-11, except real hardware.** `data/templates/ace` and the `ace-animation` example, as above: the program runs from SRAM, its own bus library and overblank program, the music player unchanged. Language "C for ACE" in *New project*, Gopher2600 at 168 MHz, tests that build both.
4. **Space report — done 2026-11-11.** The file, the SRAM and Gopher2600's SRAM, as above, in the *Space* panel, the status bar and the build's diagnostics. The cart choice was dropped: for `ACE-UF00` both carts have the same limits.
5. **Shared C.** Write vcsLib on top of the bus library, so an ELF project can add an ACE target (`make ace`) without changing its game.
6. **Hardware.** Send the file to the PlusCart, once that route exists (pending idea in `design.md`).

## Open questions

- **Is ACE worth it next to ELF?** ELF runs on the same carts and also on the Chameleon and in Stella. ACE adds full control of the bus and the M4's speed, FPU and RAM, but works in one emulator only. No source says ACE is deprecated, but the newer community work (vcsLib, PXE, TinyELF) targets ELF.
- **Real hardware:** does Juan have a PlusCart or UnoCart to try the PoC and Wolf3D on?
- **Free RAM:** no source says which SRAM and CCM a program may use besides the argument block; it would have to be asked of the UCA authors or measured on a cart.
- **Gopher2600:** the SRAM sizing for big files and the unmapped `FLASH->ACR` could be reported to its author (JetSetIlly), with Wolf3D as the example. Only if Juan wants.

## Sources

- *Strong-ARM-for-VCS*, ZackAttack (Zack Scolaro), 2017-2020 (`ace2600.h`, `vcsLib.c`, `stm32f4_flash.ld`): https://github.com/ZacharyScolaro/Strong-ARM-for-VCS. Ships an Apache-2.0 text, though GitHub lists no licence. Facts only.
- *UnoCart-2600*, Robin Edwards (original), and ZackAttack's fork with ACE (commit of 2018-06-20, `cartridge_ace.c`, `cartridge_io.h`): https://github.com/robinhedwards/UnoCart-2600, https://github.com/ZacharyScolaro/UnoCart-2600
- *United Carts of Atari*, Marco Johannes, Al_Nafuur and ZackAttack (`cartridge_emulation_ACE.c`, `cartridge_emulation_ELF.c`): https://github.com/Al-Nafuur/United-Carts-of-Atari
- *ACE-CDFJ-Driver*, Al_Nafuur, 2024 (LGPL-3.0; build flags for the M4F): https://github.com/Al-Nafuur/ACE-CDFJ-Driver
- *Gopher2600*, JetSetIlly (Stephen Illingworth), local copy `07d8de5` (`hardware/memory/cartridge/ace/`, `fingerprint.go`, `elf/elf.go`, `coprocessor/developer/dwarf/elf_shim.go`), GPL-3, read only: https://github.com/JetSetIlly/Gopher2600
- *Stella*, the Stella team: no ACE in its source or in the 7.0 binary: https://github.com/stella-emu/stella
- *PlusCart-Pico*, gtortone (GPL-3.0): https://github.com/gtortone/PlusCart-Pico; *FujiNet firmware* (`pico/atari-2600`), FujiNet team: https://github.com/FujiNetWIFI/fujinet-firmware
- *Wolf3D* ACE files (2600 port, author still to be found), in `busstuffing/roms`: header only, read with `xxd`.
- AtariAge threads, not readable by our tools (HTTP 403), so their claims are not used: *ARM scheme for UnoCart 2600* (ZackAttack), *UnoCart firmware updates*, *ACE picture and 3D demos for PlusCart* (MarcoJ).
