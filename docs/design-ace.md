# ACE cartridges — design proposal

Status: proposal (2026-11-10). Only a proof of concept exists (`tools/ace_poc`). Sources are at the end and in [sources.md](sources.md#ace-cartridges-read-2026-11-10).

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
- `ACE-UF00`: an offset from the start of the file, wherever the firmware put it. The PlusCart copies it to a flash area; the UnoCart runs it from its RAM buffer. **So UF00 code must be position-independent.**

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

The registers differ between the two carts:

- **PlusCart** (the values in Gopher2600): address on GPIOD, `0x40020C10`; data on GPIOC, `0x40020810`, `0x40020814` and `0x40020800`. A mode of `0x5555` in the low half sets the data pins as outputs.
- **UnoCart:** address on GPIOD; data on bits 8-15 of GPIOE, with mode `0x55550000`.

A UF00 program that runs on both carts must therefore take the registers from this block, and on the UnoCart also shift the data byte by 8 bits. How UCA hides that difference is still to be checked in its source. There is no official map of the RAM a program may use: the CDFJ driver overwrites `0x20000000` from offset `0x1C` onwards and uses CCM.

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

`make` builds `build/test.ace` (4,184 bytes). It is an `ACE-UF00` file whose ARM loop (a dozen instructions) serves a 4 KB DASM ROM that draws colour bars. It is linked at 0, so every symbol is an offset into the file, and it adds word 0 of the argument block to reach its data. That makes it position-independent without `-fpic`. The entry has bit 0 set, as the firmware needs.

| Run in Gopher2600 `07d8de5`, `PERFORMANCE -duration 3s` | Frames |
|---|---|
| `bars.bin`, the 4 KB ROM on its own | 1,253 |
| `test.ace` | 342 |
| the same file with the entry's bit 0 cleared | 342 |

So the 6507 runs the program that our ARM code serves. ACE is about 3.7 times slower to emulate than the plain ROM, and Gopher2600 accepts the entry with or without the Thumb bit.

Not checked yet:

- the picture itself (only the frame count);
- a real cart: the loop takes the PlusCart's 8-bit data layout, not the UnoCart's;
- why Wolf3D (`ACE-UF00`, 512 KB) produces 0 frames in the same mode. Its size is near Gopher2600's limits; this needs a look in its debugger.

`build/ACE_debugging.elf` is the same program with `-g`. Gopher2600's developer tools look for that name next to the ROM, and also for `armcode.elf`, `main.elf`, `custom2.elf` and `cdfj+.elf`, in the ROM's folder or in `main/`, `main/bin/`, `custom/bin/` or `arm/` (`coprocessor/developer/dwarf/elf_shim.go`). Source view, breakpoints in C and profiling in the debugger are not tried yet.

## What an ACE project would look like in VCSIDE

Language: **C for ARM**. `elf-c` already builds with a Makefile; ACE reuses that path with another template and `format: "ACE"`.

```
data/templates/ace/          (CC0, written for VCSIDE)
  Makefile                   cortex-m4, -mthumb, -mfpu=fpv4-sp-d16 -mfloat-abi=hard; link at 0, objcopy
  ace.ld                     header first, code, constants; .data/.bss in SRAM after the argument block
  src/header.S               the 40-byte ACE-UF00 header (size from the linker, entry with bit 0)
  src/start.c                entry: reads the argument block, copies .data, clears .bss, calls main()
  include/ace_bus.h          the bus registers taken from the argument block, read address / put byte / release
  include/vcsLib.h           the same API as the ELF template ...
  src/vcslib_ace.c           ... implemented on ace_bus (byte queue, overblank, joystick)
  src/main.c                 the ELF template's rainbow and bar, unchanged
```

- **Position independence:** variables in SRAM have fixed addresses, which is fine. The trap is pointers to constants in flash, because the file can move. The PoC's offset trick works for a handful of tables but not for C in general. The choices are `-fpic -msingle-pic-base` with a GOT that `start.c` fixes up, or a linker check that rejects absolute flash pointers. This is to be chosen in phase 3.
- **Build:** `make` writes `build/<name>.ace` and `build/ACE_debugging.elf`. The size comes from a linker symbol and the checksum stays 0, so no extra tool is needed.
- **Errors:** the ELF Makefile's errors are parsed already (gcc, ld, the checks). Add a check that the header is 40 bytes and the entry is odd.
- **Space report:** like ELF's, from `ACE_debugging.elf`'s program headers against the cart's limits (896 KB of file; SRAM and CCM free amounts to be measured).
- **Run:** Gopher2600 only, with `-mapping ACE` and `hardware.arm7.clock = 168` through its portable-mode preferences (already supported). Stella is disabled for ACE projects, with the reason given, like the TV standards an emulator lacks.
- **Debug:** breakpoints in C are not passed on today (same as ELF), but `ACE_debugging.elf` gives Gopher2600's own source-level debugger.
- **Knowledge base:** an `ACE` entry in `formats.json` (category `arm`, Gopher2600 `ACE`, no Stella id, detection by magic, the header and argument tables above), `.ace` recognised as a ROM in the project tree, and the credits in the editor help.

## Phased plan

1. **Knowledge (small).** Add `ACE` to `formats.json`. Run menu: Stella off with a note, Gopher2600 with `-mapping ACE`. Show `.ace` files with the cartridge icon and let them be run from the tree.
2. **Finish the proof of concept.** Look at the picture in Gopher2600. Try the debugger with `ACE_debugging.elf`. Find out why Wolf3D gives 0 frames. Run it on a real PlusCart (and UnoCart, with the shifted data).
3. **Template.** Build `data/templates/ace` as above, choosing how to handle position independence. Port the animated sprite example and the music export's C player to it.
4. **Space report** for ACE, and the cart choice (UnoCart / PlusCart) in project settings.
5. **Shared C.** Write vcsLib on ACE, so an ELF project can add an ACE target (`make ace`) without changing its game.
6. **Hardware.** Send the file to the PlusCart, once that route exists (pending idea in `design.md`).

## Open questions

- **Is ACE worth it next to ELF?** ELF runs on the same carts and also on the Chameleon and in Stella. ACE adds full control of the bus and the M4's speed, FPU and RAM, but works in one emulator only. No source says ACE is deprecated, but the newer community work (vcsLib, PXE, TinyELF) targets ELF.
- **Real hardware:** does Juan have a PlusCart or UnoCart to try the PoC and Wolf3D on?
- **Downloads:** to confirm the details above (UnoCart data shift, free RAM), the UCA firmware source has to be read closer. That means reading it on GitHub; nothing is downloaded or shipped.

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
