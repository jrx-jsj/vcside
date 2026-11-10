/* {{NAME}} - where the C code goes in a CDFJ cartridge
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * The code and its constants from $0800 of the ROM, after the driver, up to bank 6 (the
 * 6507's, at $7000). The variables after the display data, with the stack above them.
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(cdfj_entry)

MEMORY
{
    flash (rx) : ORIGIN = 0x00000800, LENGTH = 0x6800
    ram (rw)   : ORIGIN = 0x40001800, LENGTH = 0x600
}

SECTIONS
{
    .text :
    {
        KEEP(*(.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > flash

    /* Nothing copies initial values to RAM: give variables their values in the code. */
    .data : { *(.data .data.*) } > ram AT > flash
    ASSERT(SIZEOF(.data) == 0, "variables with initial values are not supported: set them in start() (game.c), or make them const")

    .bss (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > ram

    /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note*) }
}
