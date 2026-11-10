/* SPDX-License-Identifier: CC0-1.0 (VCSIDE ACE proof of concept) */
ENTRY(ace_entry)
SECTIONS
{
    /* Where Gopher2600 puts an ACE file, so that ACE_debugging.elf's addresses are the ones it
       runs at. The firmware may put the file elsewhere: the code only uses offsets from __ace_start. */
    . = 0x08020000;
    __ace_start = .;
    .ace_header : { KEEP(*(.ace_header)) }
    .text : { *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(4); }
    __ace_end = .;
    __ace_size = __ace_end - __ace_start;                  /* header: size of the file */
    __ace_entry_offset = (ace_entry - __ace_start) | 1;    /* header: entry, bit 0 set (Thumb) */
    /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note* .data* .bss*) }
}
