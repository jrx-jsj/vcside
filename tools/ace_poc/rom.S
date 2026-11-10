@ SPDX-License-Identifier: CC0-1.0 (VCSIDE ACE proof of concept)
        .section .rodata.vcs_rom, "a", %progbits
        .global vcs_rom
vcs_rom:
        .incbin "build/bars.bin"
