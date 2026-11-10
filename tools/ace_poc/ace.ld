/* SPDX-License-Identifier: CC0-1.0 (VCSIDE ACE proof of concept) */
ENTRY(ace_entry)
SECTIONS
{
    . = 0;
    .ace_header : { KEEP(*(.ace_header)) }
    .text : { *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(4); }
    __ace_end = .;
    /DISCARD/ : { *(.ARM.exidx* .ARM.extab* .comment .note* .data* .bss*) }
}
