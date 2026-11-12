; {{NAME}} - made with VCSIDE
; Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
; DPC+ cartridge (32K), {{TV}} timing. Build: F7 (or Build > Build). Run: F5.
;
; DPCplus.arm is DPC+'s ARM driver, copied from batari Basic when the project
; was made (batari Basic: https://github.com/batari-Basic/batari-Basic).
;
; The cartridge, as the 6507 sees it: the driver (3K), six 4K banks (the
; program starts in the last one, bank 5), the display data (4K, which the
; data fetchers read and the program can write) and the frequency table (1K,
; the notes of the cartridge's three voices). The first 128 bytes of every
; bank are the cartridge's registers, so the code starts after them.
;
; Sound: the cartridge's ARM plays three voices with 32-sample waveforms
; (in the display data) and adds them up; the 6507 copies that sum, AMPLITUDE,
; to AUDV0 once on every line, with AUDC0 = 0 (SOUND below). With no waveforms
; set, the sum is 0: the loop below already copies it, ready for music.

	processor 6502
	include "2600.h"

; ---- DPC+ registers (the ones this program uses) -----------------------------
AMPLITUDE      = $1005          ; read: the sum of the three voices, for AUDV0
FASTFETCH      = $1058          ; write: 0 = fast fetch on (lda #n, n < $28, reads register n)
BANK5          = $1FFB          ; read or write: selects bank 5

; ---- TV timing (scanlines) -------------------------------------------------
{{TVDEFS}}

; ---- SOUND: the voices' sum to AUDV0, once on every line ----------------------
	MAC SOUND
	lda AMPLITUDE
	sta AUDV0
	ENDM

; ---- RAM -------------------------------------------------------------------
	SEG.U Variables
	ORG $80
Frame   ds 1

; ---- The ARM driver --------------------------------------------------------
	SEG Code
	ORG $0400
	RORG $0000
	incbin "DPCplus.arm"

; ---- Banks 0-4: empty; a reset there goes to bank 5 --------------------------
; A bank is assembled at $F000-$FFFF (ORG: its place in the file). Writing (or
; reading) BANK0-BANK5, $1FF6-$1FFB, selects a bank: the next instruction comes
; from the new bank, at the next address.
	MAC BANK_END            ; {1}: the bank's place in the file
	ORG {1}+$0FF0
	RORG $FFF0
	sta BANK5
	jmp Reset
	ORG {1}+$0FFC
	RORG $FFFC
	.word $FFF0, $FFF0
	ENDM

	ORG $1000
	RORG $F000
	ds.b 128, 0             ; bank 0, under the cartridge's registers
	BANK_END $1000
	ORG $2000
	RORG $F000
	ds.b 128, 0             ; bank 1
	BANK_END $2000
	ORG $3000
	RORG $F000
	ds.b 128, 0             ; bank 2
	BANK_END $3000
	ORG $4000
	RORG $F000
	ds.b 128, 0             ; bank 3
	BANK_END $4000
	ORG $5000
	RORG $F000
	ds.b 128, 0             ; bank 4
	BANK_END $5000

; ---- Bank 5: the program -----------------------------------------------------
	ORG $6000
	RORG $F000
	ds.b 128, 0             ; under the cartridge's registers

Reset   sei
	cld
	ldx #0
	txa
.clear  dex
	txs
	pha
	bne .clear              ; clears RAM and TIA, S ends at $FF
	lda #$FF
	sta FASTFETCH           ; fast fetch off: lda # loads numbers

StartFrame
	lda #2
	sta WSYNC               ; VSYNC from the start of a line
	sta VBLANK
	sta VSYNC
	ldx #2
.vsync  sta WSYNC
	SOUND                   ; the sound goes on in every line
	dex
	bne .vsync
	sta WSYNC
	stx VSYNC               ; after three lines
	SOUND
	lda #(VBLANK_LINES*76-166)/64  ; timer for the vertical blank
	sta TIM64T
	inc Frame

	; the game logic goes here, with SOUND at least once a line

.vblank sta WSYNC               ; until the timer runs out, a line at a time
	SOUND
	bit TIMINT              ; D7: the timer has run out
	bpl .vblank
	sta WSYNC
	SOUND
	lda #0
	sta VBLANK              ; beam on

	ldx #VISIBLE_LINES
	ldy Frame
.kernel sta WSYNC               ; 3
	SOUND                   ; 7
	sty COLUBK              ; 3  one colour per line, scrolling down
	iny                     ; 2
	dex                     ; 2
	bne .kernel             ; 2/3

	sta WSYNC
	SOUND
	lda #0
	sta COLUBK
	lda #2
	sta VBLANK
	lda #(OVERSCAN_LINES*76-90)/64
	sta TIM64T

	; more game logic can go here

.over   sta WSYNC
	SOUND
	bit TIMINT
	bpl .over
	jmp StartFrame

	BANK_END $6000

; ---- Display data (4K): waveforms and graphics for the data fetchers ----------
	ORG $7000
	RORG $0000
	ds.b 32, 0              ; waveform 0: silence (the voices start on it)
	ORG $7FFF
	.byte 0

; ---- Frequency table (1K): the voices' notes ---------------------------------
	ORG $8000
	RORG $1000
	ds.b 4, 0               ; note 0: silence
	ORG $83FF
	.byte 0                 ; the image ends at 32K
