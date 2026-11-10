# SPDX-License-Identifier: CC0-1.0
# VCSIDE ACE proof of concept: an ACE-UF00 file whose ARM code serves a 4K 6507 ROM
# (colour bars). See docs/design-ace.md.
#
#   make        build/test.ace and build/ACE_debugging.elf (for Gopher2600's source view)
#   make run    gopher2600 RUN -mapping ACE build/test.ace
#
# Needs DASM and the Arm GNU toolchain (arm-none-eabi-gcc) in PATH.

CC     = arm-none-eabi-gcc
CFLAGS = -mcpu=cortex-m4 -mthumb -Os -g -std=c11 -Wall -Wextra -ffreestanding -nostdlib

all: build/test.ace

build/bars.bin: bars.asm | build
	dasm $< -f3 -v0 -o$@

build/ACE_debugging.elf: header.S main.c rom.S ace.ld build/bars.bin
	$(CC) $(CFLAGS) -T ace.ld header.S main.c rom.S -o $@

build/test.ace: build/ACE_debugging.elf
	arm-none-eabi-objcopy -O binary $< $@

run: build/test.ace
	gopher2600 RUN -mapping ACE $<

build:
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all run clean
