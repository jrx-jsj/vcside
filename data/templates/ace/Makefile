# {{NAME}} - ACE cartridge game made with VCSIDE (CC0: use and change it freely).
#
#   make     build/{{NAME}}.ace: the file for the PlusCart and the UnoCart (ACE-UF00),
#            and build/ACE_debugging.elf, which Gopher2600's debugger reads for the C source.
#
# Needs the Arm GNU toolchain (arm-none-eabi-gcc) in PATH. Runs in Gopher2600 (Stella has
# no ACE support).

NAME  = {{NAME}}
GAME  = build/$(NAME).ace
DEBUG = build/ACE_debugging.elf
OUT   = build/obj
CC    = arm-none-eabi-gcc
# The STM32F407's Cortex-M4 with its FPU. Gopher2600 reads DWARF 4 only (GCC writes 5), and
# none of GCC's extensions to it (-gstrict-dwarf): with them it says the DWARF is unsupported.
CFLAGS = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -O2 -gdwarf-4 -gstrict-dwarf -std=c11 \
	-Wall -Wextra -ffreestanding -fno-builtin -ffunction-sections -fdata-sections \
	-fno-tree-loop-distribute-patterns -Iinclude -Isrc

SOURCES = $(wildcard src/*.c)
OBJS    = $(SOURCES:src/%.c=$(OUT)/%.o) $(OUT)/start.o

all: $(GAME)

$(OUT)/%.o: src/%.c $(wildcard include/*.h src/*.h) | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/start.o: src/start.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

# No C library (src/memory.c has memcpy and memset); libgcc for what the code does not have.
$(DEBUG): $(OBJS) ace.ld
	$(CC) $(CFLAGS) -nostdlib -T ace.ld -Wl,--gc-sections -Wl,-Map=build/$(NAME).map \
		-Wl,--print-memory-usage -Wl,--no-warn-rwx-segments -o $@ $(OBJS) -lgcc
	@# The start code must follow the 40-byte header: the firmware's entry is an offset.
	@if [ "$$(arm-none-eabi-nm $@ | awk '$$3 == "ace_entry" {print $$1}')" != "08020028" ]; then \
		echo "$@: error: the start code must follow the 40-byte header (ace.ld)"; rm -f $@; exit 1; fi

$(GAME): $(DEBUG)
	arm-none-eabi-objcopy -O binary $< $@

$(OUT):
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all clean
