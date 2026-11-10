@ {{NAME}} - where the CDFJ driver enters the C code, and the driver's functions
@ Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
@
@ When the 6507 writes CALLFN, the driver jumps to $0800 in ARM state; the C code is Thumb.

        .syntax unified
        .cpu arm7tdmi

        .section .entry, "ax", %progbits
        .arm
        .global cdfj_entry
cdfj_entry:                     @ $0800
        add     r0, pc, #1      @ pc reads 8 ahead: $0808, and bit 0 set means Thumb
        bx      r0
        .thumb
        .thumb_func
cdfj_thumb:                     @ $0808 (where Stella starts the C code)
        push    {lr}
        bl      game_arm
        pop     {r0}
        bx      r0              @ back to the driver, in ARM state

@ The driver's functions live at $0750: voice in r2, value in r3, the address in r4. They
@ return to lr (set with "mov lr, pc": the instruction after the bx) and may change r2-r5.
        .macro  DRIVER name, address
        .section .text.\name, "ax", %progbits
        .thumb_func
        .global \name
\name:
        push    {r4, r5, lr}
        movs    r2, r0
        movs    r3, r1
        ldr     r4, =\address
        mov     lr, pc
        bx      r4
        movs    r0, r2          @ what the driver returns, if anything
        pop     {r4, r5, pc}
        .ltorg
        .endm

        DRIVER  cdfj_set_note, 0x751
        DRIVER  cdfj_reset_wave, 0x755
        DRIVER  cdfj_set_wave_size, 0x75D

@ memset and memcpy, for when the compiler calls them by itself.
        .section .text.memset, "ax", %progbits
        .thumb_func
        .global memset
memset:                         @ r0 destination, r1 value, r2 count; returns r0
        movs    r3, r0
1:      cmp     r2, #0
        beq     2f
        strb    r1, [r3]
        adds    r3, #1
        subs    r2, #1
        b       1b
2:      bx      lr

        .section .text.memcpy, "ax", %progbits
        .thumb_func
        .global memcpy
memcpy:                         @ r0 destination, r1 source, r2 count; returns r0
        push    {r4}
        movs    r3, #0
1:      cmp     r3, r2
        beq     2f
        ldrb    r4, [r1, r3]
        strb    r4, [r0, r3]
        adds    r3, #1
        b       1b
2:      pop     {r4}
        bx      lr
