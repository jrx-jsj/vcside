# {{NAME}} - CDFJ cartridge game made with VCSIDE (CC0: use and change it freely).
#
#   make     build/{{NAME}}.bin: the 32K ROM (Stella, Gopher2600, Harmony/Melody).
#            make DASMFLAGS=-DPAL=1 assembles it for PAL, if main.asm offers it.
#
# Needs DASM and the Arm GNU toolchain (arm-none-eabi-gcc) in PATH, and CDFJ's ARM
# driver as cdfj_driver.bin in this folder (2048 bytes, not included with VCSIDE).

NAME      = {{NAME}}
# The project's folders (VCSIDE passes its own): sources, what its graphics and music
# editors export, and what the build writes.
SRC_DIR       ?= src
GENERATED_DIR ?= generated
BUILD_DIR     ?= build
ROM       = $(BUILD_DIR)/$(NAME).bin
DRIVER    = cdfj_driver.bin
CC        = arm-none-eabi-gcc
DASMFLAGS ?=
# The Harmony/Melody's ARM is an ARM7TDMI: Thumb code for it.
CFLAGS = -mcpu=arm7tdmi -mthumb -Os -std=c11 -Wall -Wextra -ffreestanding -fno-builtin \
	-ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns \
	-I$(SRC_DIR) -I$(GENERATED_DIR)/gfx -I$(GENERATED_DIR)/music
INCLUDES = -I$(SRC_DIR) -I$(BUILD_DIR) -I$(GENERATED_DIR)/gfx -I$(GENERATED_DIR)/music

all: $(ROM)

$(ROM): $(DRIVER) $(SRC_DIR)/main.asm $(SRC_DIR)/2600.h $(wildcard $(GENERATED_DIR)/*/*.asm) $(BUILD_DIR)/layout.asm $(BUILD_DIR)/arm.bin
	dasm $(SRC_DIR)/main.asm -f3 -v0 $(INCLUDES) $(DASMFLAGS) -o$@ -l$(BUILD_DIR)/$(NAME).lst -s$(BUILD_DIR)/$(NAME).sym

# layout.h for DASM: "#define NAME number" becomes "NAME = number"
$(BUILD_DIR)/layout.asm: $(SRC_DIR)/layout.h | $(BUILD_DIR)
	sed -n 's/^#define \([A-Za-z0-9_]*\)  *\([0-9][0-9]*\).*/\1 = \2/p' $< > $@

SOURCES = $(SRC_DIR)/start.S $(wildcard $(SRC_DIR)/*.c)

# libgcc comes last, for what the C code does not have itself. Emulators run only Thumb
# code for CDFJ: a call into ARM code (a "_from_thumb" stub) is an error.
$(BUILD_DIR)/arm.elf: $(SOURCES) $(wildcard $(SRC_DIR)/*.h $(GENERATED_DIR)/*/*.h) arm.ld | $(BUILD_DIR)
	$(CC) $(CFLAGS) -nostdlib -T arm.ld -Wl,--gc-sections -Wl,-Map=$(BUILD_DIR)/arm.map -o $@ $(SOURCES) -lgcc
	@if arm-none-eabi-nm $@ | grep '_from_thumb$$'; then \
		echo "$@: error: the C code calls ARM code (above), which emulators do not run: write it in C"; rm -f $@; exit 1; fi

$(BUILD_DIR)/arm.bin: $(BUILD_DIR)/arm.elf
	arm-none-eabi-objcopy -O binary $< $@

$(DRIVER):
	@echo "Makefile: error: $(DRIVER) is missing. It is CDFJ's ARM driver (2048 bytes), which VCSIDE does not include:"
	@echo "copy it here from a CDFJ project (SpiceWare's CDFJ tutorial on AtariAge has cdfdriver20190317.bin)."
	@exit 1

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
