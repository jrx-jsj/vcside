# {{NAME}} - CDFJ cartridge game made with VCSIDE (CC0: use and change it freely).
#
#   make     build/{{NAME}}.bin: the 32K ROM (Stella, Gopher2600, Harmony/Melody).
#            make DASMFLAGS=-DPAL=1 assembles it for PAL, if main.asm offers it.
#
# Needs DASM and the Arm GNU toolchain (arm-none-eabi-gcc) in PATH, and CDFJ's ARM
# driver as cdfj_driver.bin in this folder (2048 bytes, not included with VCSIDE).

NAME      = {{NAME}}
ROM       = build/$(NAME).bin
DRIVER    = cdfj_driver.bin
CC        = arm-none-eabi-gcc
DASMFLAGS ?=
# The Harmony/Melody's ARM is an ARM7TDMI: Thumb code for it.
CFLAGS = -mcpu=arm7tdmi -mthumb -Os -std=c11 -Wall -Wextra -ffreestanding -fno-builtin \
	-ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns

all: $(ROM)

$(ROM): $(DRIVER) main.asm 2600.h build/layout.asm build/arm.bin
	dasm main.asm -f3 -v0 $(DASMFLAGS) -o$@ -lbuild/$(NAME).lst -sbuild/$(NAME).sym

# layout.h for DASM: "#define NAME number" becomes "NAME = number"
build/layout.asm: layout.h | build
	sed -n 's/^#define \([A-Za-z0-9_]*\)  *\([0-9][0-9]*\).*/\1 = \2/p' layout.h > $@

SOURCES = start.S $(wildcard *.c)

# libgcc comes last, for what the C code does not have itself. Emulators run only Thumb
# code for CDFJ: a call into ARM code (a "_from_thumb" stub) is an error.
build/arm.elf: $(SOURCES) $(wildcard *.h) arm.ld | build
	$(CC) $(CFLAGS) -nostdlib -T arm.ld -Wl,--gc-sections -Wl,-Map=build/arm.map -o $@ $(SOURCES) -lgcc
	@if arm-none-eabi-nm $@ | grep '_from_thumb$$'; then \
		echo "$@: error: the C code calls ARM code (above), which emulators do not run: write it in C"; rm -f $@; exit 1; fi

build/arm.bin: build/arm.elf
	arm-none-eabi-objcopy -O binary $< $@

$(DRIVER):
	@echo "Makefile: error: $(DRIVER) is missing. It is CDFJ's ARM driver (2048 bytes), which VCSIDE does not include:"
	@echo "copy it here from a CDFJ project (SpiceWare's CDFJ tutorial on AtariAge has cdfdriver20190317.bin)."
	@exit 1

build:
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all clean
