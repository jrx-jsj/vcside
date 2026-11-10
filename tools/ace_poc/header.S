@ SPDX-License-Identifier: CC0-1.0 (VCSIDE ACE proof of concept)
@ The 40-byte ACE header (ACE-UF00: the entry point is an offset from the start of the file).
        .syntax unified
        .section .ace_header, "a", %progbits
        .ascii  "ACE-UF00"
        .ascii  "VCSIDE ACE test "      @ 16 bytes
        .word   0                       @ driver version
        .word   __ace_end               @ size of the file
        .word   0                       @ checksum (not checked by the firmware)
        .word   ace_entry               @ Thumb function: the linker sets bit 0
