/* {{NAME}} - how the ACE file is laid out.
 * Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
 *
 * The file: the 40-byte header, the start code (src/start.S) and the program's image. The
 * file is linked at 0x08020000, where Gopher2600 puts ACE files, so that its debugger
 * finds build/ACE_debugging.elf's code where it runs; the firmware may put it elsewhere,
 * which the start code allows for. The program runs from SRAM, after the firmware's
 * arguments (0x20000000-0x2000002F): code, constants and variables in the first 96 KB,
 * the firmware's download buffer, which is free once the program has started. The stack
 * is the one the firmware leaves, above that buffer.
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(ace_entry)

MEMORY
{
    file (rx)  : ORIGIN = 0x08020000, LENGTH = 896K   /* the firmware's limit */
    sram (rwx) : ORIGIN = 0x20000100, LENGTH = 96K - 0x100
}

SECTIONS
{
    .ace_header : { __ace_start = .; KEEP(*(.ace_header)) } > file
    .ace_start : { KEEP(*(.ace_start)) . = ALIGN(4); } > file

    .text : ALIGN(4)
    {
        __image_start = .;
        *(.text .text.*)
        *(.RamFunc .RamFunc.*)    /* an ELF game's RAM_FUNC code (make ace in an ELF project) */
        . = ALIGN(4);
    } > sram AT > file

    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > sram AT > file

    .data : ALIGN(4)
    {
        *(.data .data.*)
        . = ALIGN(4);
        __image_end = .;
    } > sram AT > file

    .bss (NOLOAD) : ALIGN(4)
    {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > sram

    __image_offset = LOADADDR(.text) - __ace_start;
    __ace_size = LOADADDR(.data) + SIZEOF(.data) - __ace_start;
    __ace_entry_offset = (ace_entry - __ace_start) | 1;

    /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note*) }
}
