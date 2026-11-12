; {{NAME}} - made with VCSIDE
; Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
; CDFJ cartridge (32K), {{TV}} timing. Build: F7 (runs make). Run: F5.
;
; A CDFJ cartridge (Harmony/Melody) has an ARM that runs C code (game.c) when the 6507
; writes CALLFN, and data streams: "lda #stream" reads the next byte of one, in 2 cycles.
; The ARM plays three voices with 32-sample waveforms and adds them up; the 6507 copies
; that sum (the AMPLITUDE stream) to AUDV0 on every line, with AUDC0 = 0 (SOUND below).
;
; With fast fetch on, "lda #n" with n from 0 to $23 reads data stream n instead of
; loading n: load constants with ldx # and ldy #.
;
; cdfj_driver.bin is CDFJ's ARM driver (2K), which VCSIDE does not include: see the
; Makefile. The cartridge, as built: the driver, the C code from $0800 (build/arm.bin,
; up to 26K), and bank 6, the 6507's, at $7000. The display data is the cartridge's RAM.

	processor 6502
	include "2600.h"
	include "build/layout.asm"     ; the display data's offsets, from layout.h

; ---- CDFJ registers ----------------------------------------------------------
DSWRITE        = $1FF0          ; write: a byte to the display data, at DSCOMM's pointer, which moves on
DSPTR          = $1FF1          ; write twice: DSCOMM's pointer, high byte then low byte
SETMODE        = $1FF2          ; write: $F0 = fast fetch on, voices (not samples)
CALLFN         = $1FF3          ; write: $FE runs the C code, and the sound goes on meanwhile
; ---- data streams (fast fetch: lda #stream) ----------------------------------
DSCOMM         = $20            ; the display data at DSCOMM's pointer
AMPLITUDE      = $23            ; the voices' sum, for AUDV0

; ---- TV timing (scanlines) -------------------------------------------------
{{TVDEFS}}

; ---- SOUND: the voices' sum to AUDV0. Every line does it once; code that takes
; longer than a line does it in between, or the sound stops for those lines.
	MAC SOUND
	lda #AMPLITUDE
	sta AUDV0
	ENDM

; ---- RAM -------------------------------------------------------------------
	SEG.U Variables
	ORG $80
Frame   ds 1

; ---- The ARM driver and the C code -------------------------------------------
	SEG Code
	ORG $0000
	incbin "cdfj_driver.bin"
	ORG $0800
	incbin "build/arm.bin"

; ---- Bank 6: the 6507's program ----------------------------------------------
	ORG $7000
	RORG $F000

Reset
	sei
	cld
	ldx #0
	txa
.clear
	dex
	txs
	pha
	bne .clear              ; clears RAM and TIA, S ends at $FF
	ldx #$F0
	stx SETMODE             ; fast fetch on, the cartridge plays voices
	ldy #FUNCTION_START
	jsr CallArm             ; game.c: start()

StartFrame
	ldx #2
	sta WSYNC               ; VSYNC from the start of a line
	stx VBLANK
	stx VSYNC
	ldy #2
.vsync
	sta WSYNC
	SOUND                   ; the sound goes on in every line
	dey
	bne .vsync
	sta WSYNC
	sty VSYNC               ; after three lines
	SOUND
	ldx #(VBLANK_LINES*76-166)/64  ; timer for the vertical blank
	stx TIM64T
	inc Frame

	ldy #FUNCTION_FRAME
	jsr CallArm             ; game.c: frame(), the game's logic
	SOUND

	; more game logic can go here, with SOUND at least once a line

.vblank
	sta WSYNC               ; until the timer runs out, a line at a time
	SOUND
	bit TIMINT              ; D7: the timer has run out
	bpl .vblank
	sta WSYNC
	SOUND
	ldx #0
	stx VBLANK              ; beam on

	ldx #VISIBLE_LINES
	ldy Frame
.kernel
	sta WSYNC               ; 3
	SOUND                   ; 5
	sty COLUBK              ; 3  one colour per line, scrolling down
	iny                     ; 2
	dex                     ; 2
	bne .kernel             ; 2/3

	sta WSYNC
	SOUND
	ldx #0
	stx COLUBK
	ldx #2
	stx VBLANK
	ldx #(OVERSCAN_LINES*76-90)/64
	stx TIM64T

	; more game logic can go here

.over
	sta WSYNC
	SOUND
	bit TIMINT
	bpl .over
	jmp StartFrame

; ---- CallArm: Y = what to do (FUNCTION_x). The joystick and the button go to
; the display data, then the C code runs (game_arm in game.c).
CallArm
	ldx #>LAYOUT_FUNCTION
	stx DSPTR
	ldx #<LAYOUT_FUNCTION
	stx DSPTR
	sty DSWRITE             ; LAYOUT_FUNCTION
	lda SWCHA
	sta DSWRITE             ; LAYOUT_SWCHA
	lda INPT4
	sta DSWRITE             ; LAYOUT_INPT4
	ldx #$FE
	stx CALLFN              ; the 6507 waits here while the ARM runs
	rts

	ORG $7FF0
	RORG $FFF0
	ds.b 12, 0              ; the cartridge's registers
	.word Reset, Reset
