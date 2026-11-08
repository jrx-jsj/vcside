# {{NAME}} - ELF cartridge game made with VCSIDE (CC0: use and change it freely).
#
#   make             build/{{NAME}}.bin: the ROM (a relocatable ARM ELF object that the
#                    cartridge or the emulator links). Stella, Gopher2600, PlusCart, UnoCart.
#   make chameleon   build/{{NAME}}-chameleon.elf: the game linked with the Chameleon Cart
#                    firmware, to flash with an ST-Link. Needs the firmware objects of the
#                    Chameleon's ELF template (not included): CHAMELEON_FW=<its chameleon_cart folder>.
#
# Needs the Arm GNU toolchain (arm-none-eabi-gcc) in PATH.

NAME  = {{NAME}}
GAME  = build/$(NAME).bin
OUT   = build/elf
CC    = arm-none-eabi-gcc
# Cortex-M0 code runs on every ELF cartridge (and is what Stella emulates).
# -fdata-sections without -ffunction-sections: constants without pointers stay in flash on
# the PlusCart, and the code stays in one section per file.
CFLAGS = -mcpu=cortex-m0 -mthumb -O2 -std=c11 -Wall -Wextra -ffreestanding -fno-builtin \
         -fno-common -fdata-sections -Iinclude

SOURCES = $(wildcard src/*.c)
OBJS    = $(SOURCES:src/%.c=$(OUT)/%.o) $(OUT)/memcpy_long.o

all: $(GAME)

# memcpy/memset that the compiler calls by itself go through the long jumps of memcpy_long.S.
$(OUT)/%.o: src/%.c $(wildcard include/*.h src/*.h) | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
	arm-none-eabi-objcopy --redefine-sym memcpy=memcpy_long --redefine-sym memset=memset_long $@

$(OUT)/memcpy_long.o: src/memcpy_long.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

# Partial link with libgcc (the M0 has no divide instruction), then only what relocation
# needs. libgcc's divisions are renamed: some emulator versions replace __aeabi_*div* with
# their own. Big games should also merge their sections afterwards (Gopher2600 looks each
# memory access up section by section).
DIVISIONS = $(foreach f,idiv uidiv idivmod uidivmod,--redefine-sym __aeabi_$(f)=libgcc_$(f))
$(GAME): $(OBJS)
	$(CC) $(CFLAGS) -r -nostdlib -o $@ $^ -lgcc
	arm-none-eabi-strip --strip-unneeded $@
	arm-none-eabi-objcopy $(DIVISIONS) $@
	@# Only the loader's functions may stay undefined: Stella links unknown ones to 0 and the ARM hangs.
	@# Each one is reported with the source file that uses it.
	@bad=$$(arm-none-eabi-nm $@ | awk '$$1 == "U" {print $$2}' | grep -v -E '^(vcs[A-Za-z0-9]+|memset|memcpy|randint|ColorLookup|ReverseByte)$$'); \
	if [ -n "$$bad" ]; then \
		for s in $$bad; do for o in $$(arm-none-eabi-nm -A $(OBJS) | grep " U $$s$$" | cut -d: -f1); do \
			echo "$$o" | sed "s|^$(OUT)/\(.*\)\.o$$|src/\1.c: error: undefined symbol '$$s' (only vcsLib functions may be left undefined)|"; \
		done; done; rm -f $@; exit 1; fi
	@# Calls to the loader must be long (vcs.h between #pragma long_calls ... long_calls_off).
	@if arm-none-eabi-readelf -r -W $@ | awk '$$3 ~ /THM_CALL|THM_JUMP24/' | grep -E ' (vcs[A-Za-z0-9]+|memset|memcpy|randint)$$'; then \
		echo "$@: error: short calls to the loader (above): declare them between #pragma long_calls and long_calls_off"; rm -f $@; exit 1; fi

# Chameleon Cart (STM32G070RB: 128 KB of flash and 36 KB of RAM, firmware included).
CHAMELEON_FW ?=
GAME_CC = build/$(NAME)-chameleon.elf
chameleon: $(GAME_CC)
$(GAME_CC): $(GAME) chameleon/support.c
	@if [ -z "$(CHAMELEON_FW)" ]; then echo "Makefile: error: set CHAMELEON_FW to the chameleon_cart folder of the Chameleon's ELF template"; exit 1; fi
	mkdir -p $(OUT)/nolibc
	for l in libc libm libgcc; do arm-none-eabi-ar rc $(OUT)/nolibc/$$l.a; done
	sed 's/^_Min_Heap_Size = 0x200;/_Min_Heap_Size = 0;/' $(CHAMELEON_FW)/STM32G070RBTX_FLASH.ld > $(OUT)/chameleon.ld
	$(CC) -mcpu=cortex-m0plus -mthumb -O2 -ffreestanding -fno-builtin -c chameleon/support.c -o $(OUT)/support.o
	$(CC) $(filter-out %/syscalls.o %/sysmem.o,$(wildcard $(CHAMELEON_FW)/*.o)) $(OUT)/support.o $(GAME) \
		-o $@ -mcpu=cortex-m0plus -mthumb -nostdlib -static -L$(OUT)/nolibc -T $(OUT)/chameleon.ld \
		-Wl,--gc-sections,--print-memory-usage

$(OUT):
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all chameleon clean
