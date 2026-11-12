; {{NAME}} - made with VCSIDE
; Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
; {{FORMAT}} cartridge ({{BANKS}} banks of 4K), {{TV}} timing. Build: F7 (or Build > Build). Run: F5.
;
; Banks: the 2600 sees 4K of the cartridge at a time, at $F000-$FFFF. Touching
; a hotspot address (reading or writing it) shows another bank: bank n is
; BANK0_HOTSPOT+n. The switch happens between two instructions, and the next
; one is fetched from the new bank at the next address, so the code that
; switches sits at the same address in both banks.
; The cartridge can start in any bank: every bank ends the same way (BANK_END),
; with the vectors on a switch to bank 0 and a jump to Reset.
; Bank 0 has the frame loop; bank 1 has FrameLogic, called from bank 0 with
; jsr CallBank1. The other banks are empty: copy CallBank1's trampoline (in
; BANK_END) to call into them.

BANKS         = {{BANKS}}
BANK0_HOTSPOT = {{HOTSPOT}}     ; bank 0; bank 1 is BANK0_HOTSPOT+1, and so on

	processor 6502
	include "2600.h"

; ---- TV timing (scanlines) -------------------------------------------------
{{TVDEFS}}

; ---- RAM -------------------------------------------------------------------
	SEG.U Variables
	ORG $80
Frame   ds 1
Colour  ds 1                    ; the first line's colour (FrameLogic, bank 1)

; ---- The end of every bank: the same code at the same addresses -------------
; {1}: the bank's number.
	MAC BANK_END
	IF . > $FFE0
		ECHO "Bank", [{1}]d, "is full: its code runs into $FFE0"
		ERR
	ENDIF
	ORG [{1}*$1000]+$FE0
	RORG $FFE0
	sta BANK0_HOTSPOT       ; $FFE0: power on, in any bank: bank 0, which
	jmp Reset               ; $FFE3: goes on here
	IF {1} == 0
CallBank1                       ; $FFE6: jsr CallBank1 runs FrameLogic in bank 1
	sta BANK0_HOTSPOT+1     ; $FFE6: bank 1 goes on at $FFE9
	ds 6, $EA               ; (bank 1's code is here)
	rts                     ; $FFEF: back to the caller, in bank 0
	ENDIF
	IF {1} == 1
	ds 3, $EA               ; (bank 0's code is here)
	jsr FrameLogic          ; $FFE9
	sta BANK0_HOTSPOT       ; $FFEC: bank 0 goes on at $FFEF
	ENDIF
	ORG [{1}*$1000]+$FFC
	RORG $FFFC
	.word $FFE0             ; RESET
	.word $FFE0             ; IRQ/BRK
	ENDM

; ============================================================================
; Bank 0: the frame loop
; ============================================================================
	SEG Code
	ORG $0000
	RORG $F000

Reset   sei
	cld
	ldx #0
	txa
.clear  dex
	txs
	pha
	bne .clear              ; clears RAM and TIA, S ends at $FF

StartFrame
	lda #2
	sta VBLANK
	sta VSYNC
	sta WSYNC
	sta WSYNC
	sta WSYNC
	lda #0
	sta VSYNC

	lda #(VBLANK_LINES*76-14)/64  ; timer for the vertical blank
	sta TIM64T
	jsr CallBank1           ; the frame's logic, in bank 1
.vblank lda INTIM
	bne .vblank
	sta WSYNC
	sta VBLANK              ; A = 0: beam on

	ldx #VISIBLE_LINES
	lda Colour
.kernel sta WSYNC               ; 3
	sta COLUBK              ; 3  one colour per line, scrolling down
	adc #1                  ; 2
	dex                     ; 2
	bne .kernel             ; 2/3

	lda #2
	sta VBLANK
	lda #(OVERSCAN_LINES*76-14)/64
	sta TIM64T
.over   lda INTIM
	bne .over
	jmp StartFrame

	BANK_END 0

; ============================================================================
; Bank 1: the game's logic
; ============================================================================
	ORG $1000
	RORG $F000

; ---- FrameLogic: once a frame, in the vertical blank. Returns to bank 0.
FrameLogic
	inc Frame
	lda Frame
	sta Colour
	rts

	BANK_END 1

; ============================================================================
; Banks 2 on: empty
; ============================================================================
BANK    SET 2
	REPEAT BANKS-2
	ORG BANK*$1000
	RORG $F000
	BANK_END BANK
BANK    SET BANK+1
	REPEND
