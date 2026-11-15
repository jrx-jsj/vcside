# TinyELF Basic, as VCSIDE builds it

These are the sources of [TinyELF Basic](https://github.com/Al-Nafuur/TinyELF-Basic) by
Al_Nafuur: a BASIC machine that runs as an ELF game on the PlusCart and the UnoCart, with a
36-column text screen, graphics, sound and a KeyPortari keyboard. VCSIDE's "TinyELF Basic"
projects build them into a ROM that holds the project's BASIC program and runs it at
power-on.

- **Upstream:** commit `2b8a5c3` of the `main` branch (after v0.20).
- **Changed by VCSIDE:** the autorun option, in `autorun.patch` (`main.c` and the Makefile;
  the Makefile is not copied here, VCSIDE runs the compiler itself). With
  `TINYELF_AUTORUN` defined as the path of a listing, the listing is put into the ROM
  (`.incbin`), entered at power-on through the same tokenizer as `ENTER`, and run. Without
  it, the sources build TinyELF Basic as released.
- **Licence:** GNU General Public License version 3 (`LICENSE`); the credits are in
  `NOTICE`. TinyELF Basic is built on Stefan Lenz's Basic2, its text screen is
  Omegamatrix's 36-character kernel, `i2c_savekey.c` ports Alex Herbert's and Thomas
  Jentzsch's SaveKey driver, and `vcsLib.h` is Zachary Scolaro's.

VCSIDE builds them with the Arm GNU Toolchain (it needs its C library, newlib):

```
arm-none-eabi-gcc -r -mlong-calls -fno-exceptions -fno-builtin-memmove -march=armv7-m -Os -Wall \
  -Wl,-T,tinyelf.ld -DTINYELF_AUTORUN='"<project>/src/main.bas"' *.c -o <project>/build/<name>.bin -lgcc
```
