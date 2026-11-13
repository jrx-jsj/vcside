; {{NAME}} - made with VCSIDE
; Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
; DPC+ cartridge (32K), {{TV}} timing. Build: F7 (or Build > Build). Run: F5.
;
; Example: music played by the cartridge. A DPC+ cartridge has an ARM that plays
; three voices with 32-sample waveforms and adds them up; the 6507 only copies
; that sum (AMPLITUDE) to AUDV0 on every line, with AUDC0 = 0. theme.vcsmusic
; (open it from the project panel) is the song, played as samples with three
; voices, and its two sound effects; saving it writes music.asm, which is
; included three times below: the player and the song in the code, the
; waveforms in the display data, the frequencies in the frequency table.
;
; The joystick moves the square; the fire button plays the "fire" effect and
; reaching an edge plays "bump". "fire" takes the third voice while it
; sounds; "bump" plays on the TIA's channel 1, as a TIA sound (MusicFrame writes
; AUDC1, AUDF1 and AUDV1). The background shows the sound: each line's luminance is the
; amplitude written to AUDV0 on it.
;
; DPCplus.arm is DPC+'s ARM driver, copied from batari Basic when the project
; was made (batari Basic: https://github.com/batari-Basic/batari-Basic).
;
; The cartridge, as the 6507 sees it: the driver (3K), six 4K banks (this
; program uses only the last one, where DPC+ starts), the display data (4K)
; and the frequency table (1K). The first 128 bytes of every bank are the
; cartridge's registers, so the code starts after them.

	processor 6502
	include "2600.h"

; ---- DPC+ registers (only the music's) ---------------------------------------
AMPLITUDE      = $1005          ; read: the sum of the three voices, for AUDV0
WAVEFORM0      = $105D          ; write: a voice's waveform, a 32-byte slot of the display data
WAVEFORM1      = $105E
WAVEFORM2      = $105F
NOTE0          = $1075          ; write: a voice's entry in the frequency table
NOTE1          = $1076
NOTE2          = $1077
BANK5          = $1FFB          ; read: selects the last bank

; ---- TV timing (scanlines) -------------------------------------------------
{{TVDEFS}}
HERO_HEIGHT    = 12             ; lines
HERO_RIGHT     = 149            ; the last x: PosObject draws 3 pixels further right

; ---- SOUND: the voices' sum to AUDV0. Every line does it once; code that takes
; longer than a line does it in between, or the sound stops for those lines.
	MAC SOUND
	lda AMPLITUDE
	sta AUDV0
	ENDM

; ---- RAM -------------------------------------------------------------------
	SEG.U Variables
	ORG $80
HeroX   ds 1            ; colour clocks from the left
HeroY   ds 1            ; lines from the top of the screen
Button  ds 1            ; the fire button on the last frame (0 = down)
Bump    ds 1            ; not 0: the square has just reached an edge
MusicRam ds 29          ; the music player's: 5 bytes, 7 per voice, 3 for the TIA's effects
MusicRamEnd
MUSIC_RAM = MusicRam

; ---- The ARM driver --------------------------------------------------------
	SEG Code
	ORG $0400
	RORG $0000
	incbin "DPCplus.arm"

; ---- Banks 0-4: unused; a reset there goes to bank 5 -------------------------
	ORG $1FF0
	RORG $FFF0
	sta BANK5               ; the next instruction comes from bank 5, at the same address
	jmp Reset
	ORG $1FFC
	RORG $FFFC
	.word $FFF0, $FFF0
	ORG $2FF0
	RORG $FFF0
	sta BANK5
	jmp Reset
	ORG $2FFC
	RORG $FFFC
	.word $FFF0, $FFF0
	ORG $3FF0
	RORG $FFF0
	sta BANK5
	jmp Reset
	ORG $3FFC
	RORG $FFFC
	.word $FFF0, $FFF0
	ORG $4FF0
	RORG $FFF0
	sta BANK5
	jmp Reset
	ORG $4FFC
	RORG $FFFC
	.word $FFF0, $FFF0
	ORG $5FF0
	RORG $FFF0
	sta BANK5
	jmp Reset
	ORG $5FFC
	RORG $FFFC
	.word $FFF0, $FFF0

; ---- Bank 5: the program -----------------------------------------------------
	ORG $6000
	RORG $F000
	ds.b 128, 0             ; under the cartridge's registers

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
	lda #76
	sta HeroX
	lda #VISIBLE_LINES/2
	sta HeroY
	lda #$80
	sta Button              ; as if up: holding it at power on plays nothing
	lda #$1E                ; yellow square
	sta COLUP0
	jsr MusicInit           ; the song from its start (AUDC0 is 0 already)

StartFrame
	lda #2
	sta VBLANK
	sta VSYNC
	ldx #3
.vsync
	sta WSYNC
	lda AMPLITUDE           ; the sound goes on in every line
	sta AUDV0
	dex
	bne .vsync
	stx VSYNC
	lda #(VBLANK_LINES*76-14)/64  ; timer for the vertical blank
	sta TIM64T

	jsr Joystick
	SOUND
	jsr Sounds
	SOUND
	jsr MusicFrame          ; the voices' waveforms and notes for this frame
	SOUND
	lda HeroX
	ldx #0                  ; player 0
	jsr PosObject
	sta WSYNC
	sta HMOVE
	SOUND
.vblank
	sta WSYNC               ; until the timer runs out, a line at a time
	lda AMPLITUDE
	sta AUDV0
	lda INTIM
	bne .vblank
	sta VBLANK              ; A = 0: beam on

; ---- Kernel: one line per pass ------------------------------------------------
	ldx #0                  ; the line, from the top
.line
	sta WSYNC
	lda AMPLITUDE           ; 4
	sta AUDV0               ; 3
	asl                     ; 2  the amplitude (0-15) as luminance
	and #$0E                ; 2
	ora #$80                ; 2  in blue
	sta COLUBK              ; 3
	txa                     ; 2
	sec                     ; 2
	sbc HeroY               ; 3
	cmp #HERO_HEIGHT        ; 2
	lda #0                  ; 2
	bcs .draw               ; 2/3
	lda #$FF                ; 2  inside the square
.draw
	sta GRP0                ; 3
	inx                     ; 2
	cpx #VISIBLE_LINES      ; 2
	bne .line               ; 3

	sta WSYNC
	SOUND
	lda #0
	sta COLUBK
	sta GRP0
	lda #2
	sta VBLANK
	lda #(OVERSCAN_LINES*76-14)/64
	sta TIM64T
.over
	sta WSYNC
	lda AMPLITUDE
	sta AUDV0
	lda INTIM
	bne .over
	jmp StartFrame

; ---- Joystick: the square moves in four directions -------------------------------
Joystick
	lda SWCHA
	bmi .noRight            ; D7 = 0: right
	ldx HeroX
	cpx #HERO_RIGHT
	bcs .noRight
	inx
	stx HeroX
	cpx #HERO_RIGHT
	bne .noRight
	stx Bump                ; just got to the right edge
.noRight
	lda SWCHA
	asl
	bmi .noLeft             ; D6 = 0: left
	ldx HeroX
	beq .noLeft
	dex
	stx HeroX
	bne .noLeft
	inc Bump                ; just got to the left edge
.noLeft
	lda SWCHA
	asl
	asl
	bmi .noDown             ; D5 = 0: down
	ldx HeroY
	cpx #VISIBLE_LINES-HERO_HEIGHT
	bcs .noDown
	inx
	stx HeroY
.noDown
	lda SWCHA
	asl
	asl
	asl
	bmi .noUp               ; D4 = 0: up
	ldx HeroY
	beq .noUp
	dex
	stx HeroY
.noUp
	rts

; ---- Sounds: the effects, when the button goes down and at the edges -------
Sounds
	lda INPT4
	and #$80                ; D7 = 0: the button is down
	cmp Button              ; carry clear: down now, up before
	sta Button
	bcs .noFire
	lda #MUSIC_EFFECT_FIRE
	jsr MusicEffect
.noFire
	lda Bump
	beq .done
	lda #0
	sta Bump
	lda #MUSIC_EFFECT_BUMP
	jsr MusicEffect
.done
	rts

; ---- PosObject: A = x (0-159), X = object (0 = P0). Ends with HMxx set. ------
PosObject
	sta WSYNC
	sec
.div
	sbc #15
	bcs .div
	eor #7
	asl
	asl
	asl
	asl
	sta HMP0,x
	sta RESP0,x
	rts

; the player and the song
MUSIC_PART SET 1
	include "music.asm"
	IF MusicRam + MUSIC_RAM_BYTES > MusicRamEnd
	ERR                     ; MusicRam is too small: make it MUSIC_RAM_BYTES long
	ENDIF

	ORG $6FF0
	RORG $FFF0
	sta BANK5               ; as in the other banks
	jmp Reset
	ORG $6FFC
	RORG $FFFC
	.word $FFF0, $FFF0

; ---- Display data (4K): the waveforms --------------------------------------
	ORG $7000
	RORG $D000
MUSIC_PART SET 2
	include "music.asm"

; ---- Frequency table (1K) ------------------------------------------------------
	ORG $8000
	RORG $1000
MUSIC_PART SET 3
	include "music.asm"
	ORG $83FF
	.byte 0                 ; the image ends at 32K
