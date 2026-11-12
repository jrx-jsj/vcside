/*
	* Long jumps to the loader's memcpy and memset (CC0, VCSIDE's ELF template).
	*
	* The compiler calls memcpy/memset by itself (struct copies, initialisers) with a short
	* BL, which cannot reach the loader on the PlusCart. The Makefile renames those calls to
	* memcpy_long/memset_long in every object; these jump through an absolute address that
	* the loader fills in. r3 is free in both.
	*/
	.syntax unified
	.thumb
	.text

	.global memcpy_long
	.type memcpy_long, %function
	.thumb_func
memcpy_long:
	ldr r3, 1f
	bx r3
	.align 2
1:	.word memcpy

	.global memset_long
	.type memset_long, %function
	.thumb_func
memset_long:
	ldr r3, 2f
	bx r3
	.align 2
2:	.word memset
