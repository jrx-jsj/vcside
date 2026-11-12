@ {{NAME}} - the ACE header and the code that starts the program.
@ Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
@
@ The firmware copies the file to its flash, wherever there is room, and calls the entry
@ point with no arguments (they are at 0x20000000, see ace.h). The program itself is
@ linked to run from the ARM's SRAM at 0x20000100, so this code, which runs from the
@ file, uses only offsets from its start: it asks the firmware to
@ reboot the console into this cartridge, copies the program to SRAM, clears its
@ variables and calls main(). The firmware serves its menu from that SRAM until the
@ reboot, so the copy must come after it. If main returns non-zero, it goes back to the
@ firmware's menu.

	.syntax unified
	.cpu    cortex-m4
	.thumb

	.section .ace_header, "a", %progbits
	.ascii  "ACE-UF00"                  @ any cart with the unified firmware
	.ascii  "{{ACE_NAME}}"              @ the name: 16 characters, with spaces
	.word   0                           @ version of the program
	.word   __ace_size                  @ size of the file (ace.ld)
	.word   0                           @ checksum: the firmware does not check it
	.word   __ace_entry_offset          @ entry: offset in the file, bit 0 set (ace.ld)

	.section .ace_start, "ax", %progbits
	.global ace_entry
	.thumb_func
ace_entry:
	push    {r4, lr}
	ldr     r0, =0x20000000
	ldr     r0, [r0, #8]                @ reboot_into_cartridge()
	blx     r0
	ldr     r0, =0x20000000
	ldr     r0, [r0]                    @ where the firmware put this file
	ldr     r1, =__image_offset         @ the program's offset in the file (a number)
	add     r0, r1
	ldr     r1, =__image_start          @ SRAM
	ldr     r2, =__image_end
1:      cmp     r1, r2
	bhs     2f
	ldr     r3, [r0], #4
	str     r3, [r1], #4
	b       1b
2:      ldr     r1, =__bss_start
	ldr     r2, =__bss_end
	movs    r3, #0
3:      cmp     r1, r2
	bhs     4f
	str     r3, [r1], #4
	b       3b
4:      ldr     r0, =main                   @ in SRAM: an address, not an offset
	blx     r0
	cbz     r0, 5f
	@ main asked to go back to the menu (after ace_leave): the firmware's own cartridge
	@ takes the bus from here, called from this code in the file because the menu uses
	@ the SRAM where the program was. Its result goes back to the firmware.
	ldr     r0, =0x20000000
	ldr     r0, [r0, #12]               @ back_to_menu()
	blx     r0
5:      pop     {r4, pc}
	.ltorg
