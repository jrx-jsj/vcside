; {{NAME}} - made with VCSIDE
; Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
; VCSIDE The Game on a CDFJ cartridge (32K), {{TV}} at start.
; Build: F7 (runs make). Run: F5.
;
; A room with walls where the hero walks and shoots at two enemies. Hitting
; one scores a point; touching one costs a life. A title screen with a menu
; (PLAY, and the TV system) comes first; with no lives left, or with RESET,
; the game goes back to it.
;
; The work is split: the game's logic, the music and what each line shows are
; C run by the cartridge's ARM (game.c), once a frame; this 6507 program only
; draws. A CDFJ cartridge (Harmony/Melody) runs the C code when the 6507
; writes CALLFN, and has data streams: "lda #stream" reads the next byte of
; one, in 2 cycles (fast fetch). The C code fills them for the frame: a byte
; per line (or per pair of lines) for each register the kernels write.
; The ARM also plays three voices with 32-sample waveforms and adds them up;
; the 6507 copies that sum (the AMPLITUDE stream) to AUDV0 on every line, with
; AUDC0 = 0.
;
; With fast fetch on, "lda #n" with n from 0 to $23 reads data stream n
; instead of loading n: this program loads its constants with ldx # and ldy #.
;
; cdfj_driver.bin is CDFJ's ARM driver (2K), which VCSIDE does not include:
; see the Makefile. The cartridge, as built: the driver, the C code from $0800
; (build/arm.bin, up to 26K), and bank 6, the 6507's, at $7000. The display
; data is the cartridge's RAM.
;
; The frame: VSYNC, the vertical blank (the C code, then the objects' places
; and registers it gives), 192 visible lines (the title's kernel or the
; game's), and the overscan (the collision registers, for the C code). PAL
; has 25 more blank lines above and below the same 192 (312 in all); NTSC and
; PAL60 have 262.

	processor 6502
	include "2600.h"
	include "layout.asm"        ; the display data's offsets and the streams, from layout.h

; ---- CDFJ registers ----------------------------------------------------------
DSWRITE        = $1FF0          ; write: a byte to the display data, at DSCOMM's pointer, which moves on
DSPTR          = $1FF1          ; write twice: DSCOMM's pointer, high byte then low byte
SETMODE        = $1FF2          ; write: $F0 = fast fetch on, voices (not samples)
CALLFN         = $1FF3          ; write: $FE runs the C code, and the sound goes on meanwhile
DSCOMM         = $20            ; data stream: the display data at DSCOMM's pointer
AMPLITUDE      = $23            ; data stream: the voices' sum, for AUDV0

; ---- Settings ----------------------------------------------------------------
DEFAULT_SYSTEM = {{TV_SYSTEM}}  ; the TV system at start (the title's menu changes it): 0 NTSC, 1 PAL60, 2 PAL
ROOM_PAIRS     = 88             ; the room's line pairs (room.vcsgfx: 22 rows of 8 lines)
CART_LINES     = 90             ; the title's cartridge (title.vcsgfx: 45 rows of 2)...
TEXT48_LINES   = 14             ; ...THE GAME...
TEXT_LINES     = 15             ; ...and the menu's texts (5 rows of 3): game.c fills that many

; ---- SOUND: the voices' sum to AUDV0. Every line does it once; code that takes
; longer than a line does it in between, or the sound stops for those lines.
	MAC SOUND
	lda #AMPLITUDE
	sta AUDV0
	ENDM

; DRAW_TEXT first: TEXT_LINES lines of a text of title.vcsgfx from six
; streams (first to first+5). Its rows are 40 columns (asymmetric): each line
; writes PF0-PF2 for the left half and again, in time, for the right one.
	MAC DRAW_TEXT
	ldx #TEXT_LINES
.line
	sta WSYNC
	lda #AMPLITUDE          ; 2
	sta AUDV0               ; 5
	lda #{1}                ; 7
	sta PF0                 ; 10
	lda #{1}+1              ; 12
	sta PF1                 ; 15
	lda #{1}+2              ; 17
	sta PF2                 ; 20
	lda #{1}+3              ; 22
	bit Temp                ; 25
	sta PF0                 ; 28  the right PF0: from cycle 28 to 49
	lda #{1}+4              ; 30
	nop                     ; 32
	nop                     ; 34
	nop                     ; 36
	sta PF1                 ; 39  the right PF1: from cycle 39 to 54
	lda #{1}+5              ; 41
	nop                     ; 43
	nop                     ; 45
	nop                     ; 47
	sta PF2                 ; 50  the right PF2: from cycle 50 to 65
	dex                     ; 52
	bne .line               ; 55
	sta WSYNC
	SOUND
	ldx #0
	stx PF0
	stx PF1
	stx PF2
	ENDM

; ---- RAM -------------------------------------------------------------------
	SEG.U Variables
	ORG $80
System    ds 1          ; 0 NTSC, 1 PAL60, 2 PAL: game.c changes it, the 6507 keeps it
Mode      ds 1          ; 0 the title screen, 1 the game: which kernel draws
Collided  ds 4          ; CXP0FB, CXP1FB, CXBLPF and CXPPMM of the frame drawn last
Temp      ds 1
Temp2     ds 1

; ---- The ARM driver and the C code -------------------------------------------
	SEG Code
	ORG $0000
	incbin "cdfj_driver.bin"
	ORG $0800
	incbin "arm.bin"

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
	ldx #DEFAULT_SYSTEM
	stx System
	ldy #FUNCTION_START
	jsr CallArm             ; the song from its start, the title (AUDC0 is 0 already)

StartFrame SUBROUTINE
	ldx #2
	sta WSYNC               ; VSYNC from the start of a line
	stx VBLANK
	stx VSYNC
.vsync
	sta WSYNC
	SOUND                   ; the sound goes on in every line
	dex
	bne .vsync
	sta WSYNC
	stx VSYNC               ; after three lines
	SOUND
	ldx System
	lda VblankTime,x        ; the vertical blank's length for the TV system
	sta TIM64T

	ldy #FUNCTION_FRAME
	jsr CallArm             ; the frame: logic, music, the streams
	SOUND
	lda #DSCOMM             ; what the C code left, in send_registers' order
	sta Mode
	lda #DSCOMM
	sta System
	ldx #0                  ; the objects' places: P0, P1, M0, M1 and the ball
.place
	lda #DSCOMM
	jsr PosObject
	inx
	cpx #5
	bne .place
	sta WSYNC
	sta HMOVE
	SOUND
	lda #DSCOMM
	sta REFP0
	lda #DSCOMM
	sta NUSIZ0
	lda #DSCOMM
	sta NUSIZ1
	lda #DSCOMM
	sta VDELP0
	lda #DSCOMM
	sta VDELP1
	lda #DSCOMM
	sta CTRLPF
	lda #DSCOMM
	sta COLUPF
	lda #DSCOMM
	sta COLUBK
	lda #DSCOMM
	sta COLUP0
	lda #DSCOMM
	sta COLUP1
	lda #DSCOMM             ; a sound effect on the TIA's channel 1
	sta AUDC1
	lda #DSCOMM
	sta AUDF1
	lda #DSCOMM
	sta AUDV1
	ldx #0                  ; nothing drawn until the kernel
	stx GRP0
	stx GRP1
	stx GRP0
	stx ENAM0
	stx ENAM1
	stx ENABL
.wait
	sta WSYNC               ; until the timer runs out, a line at a time
	SOUND
	bit TIMINT              ; D7: the timer has run out (it stays set)
	bpl .wait
	lda Mode
	bne .game
	jsr TitleKernel
	jmp .overscan
.game
	jsr GameKernel

.overscan
	ldx #2
	stx VBLANK
	sta WSYNC               ; the timer from the start of a line: always the same length
	SOUND
	ldx System
	lda OverscanTime,x
	sta TIM64T
	lda CXP0FB              ; what touched what, for the C code (game.c, collisions)
	sta Collided
	lda CXP1FB
	sta Collided+1
	lda CXBLPF
	sta Collided+2
	lda CXPPMM
	sta Collided+3
	sta CXCLR
.over
	sta WSYNC
	SOUND
	bit TIMINT
	bpl .over
	jmp StartFrame

; Timer values (TIM64T, 64 cycles each) for the blank lines of each TV system:
; 262 lines (NTSC, PAL60) and 312 (PAL: 25 more above and below). Measured: the
; vertical blank's work (the C code, the registers) must end before its timer.
VblankTime      .byte 42, 42, 72
OverscanTime    .byte 33, 33, 62

; ---- CallArm: Y = what to do (FUNCTION_x). The inputs go to the display data
; (layout.h, LAYOUT_FUNCTION on), then the C code runs (game_arm); DSCOMM is left
; on what it writes back (LAYOUT_OUT).
CallArm SUBROUTINE
	ldx #>LAYOUT_FUNCTION
	stx DSPTR
	ldx #<LAYOUT_FUNCTION
	stx DSPTR
	sty DSWRITE             ; LAYOUT_FUNCTION
	lda SWCHA
	sta DSWRITE             ; LAYOUT_SWCHA
	lda INPT4
	sta DSWRITE             ; LAYOUT_INPT4
	lda SWCHB
	sta DSWRITE             ; LAYOUT_SWCHB
	lda System
	sta DSWRITE             ; LAYOUT_SYSTEM
	ldx #0
.collided
	lda Collided,x
	sta DSWRITE             ; LAYOUT_CXP0FB to LAYOUT_CXPPMM
	inx
	cpx #4
	bne .collided
	ldx #$FE
	stx CALLFN              ; the 6507 waits here while the ARM runs
	rts

; ============================================================================
; Kernels: every register write comes from a stream
; ============================================================================

; ---- TitleKernel: 192 lines: VCSIDE's cartridge, THE GAME, PLAY and the TV
; system (game.c, title_streams).
TitleKernel SUBROUTINE
	jsr BeamOn
	ldx #10
	jsr BlankLines
	jsr Cartridge
	lda #DSCOMM             ; THE GAME's colour
	sta COLUP0
	sta COLUP1
	ldx #$03                ; the players: three close copies each
	stx NUSIZ0
	stx NUSIZ1
	ldx #8
	jsr BlankLines
	jsr Text48
	ldx #16
	jsr BlankLines
	lda #DSCOMM             ; PLAY's colour: white when selected, else grey
	sta COLUPF
	DRAW_TEXT STREAM_PLAY
	ldx #8
	jsr BlankLines
	lda #DSCOMM             ; the TV system's
	sta COLUPF
	DRAW_TEXT STREAM_SYSTEM
	ldx #192-10-[CART_LINES+1]-8-[TEXT48_LINES+1]-16-[TEXT_LINES+1]-8-[TEXT_LINES+1]
	jmp BlankLines

; ---- Cartridge: the cartridge of VCSIDE's icon: the row's colour, its
; asymmetric playfield, and the label's grey sides, which are the two missiles
; (a playfield line has one colour), written before the left one (cycle 35).
Cartridge SUBROUTINE
	ldx #CART_LINES
.line
	sta WSYNC
	lda #AMPLITUDE          ; 2
	sta AUDV0               ; 5
	lda #STREAM_CART_COLOUR ; 7
	sta COLUPF              ; 10
	lda #STREAM_CART        ; 12
	sta PF0                 ; 15
	lda #STREAM_CART+1      ; 17
	sta PF1                 ; 20
	lda #STREAM_CART_SIDES  ; 22
	sta ENAM0               ; 25
	sta ENAM1               ; 28
	lda #STREAM_CART+2      ; 30
	sta PF2                 ; 33
	lda #STREAM_CART+3      ; 35
	sta PF0                 ; 38  the right PF0: from cycle 28 to 49
	lda #STREAM_CART+4      ; 40
	sta PF1                 ; 43  the right PF1: from cycle 39 to 54
	lda #STREAM_CART+5      ; 45
	nop                     ; 47
	sta PF2                 ; 50  the right PF2: from cycle 50 to 65
	dex                     ; 52
	bne .line               ; 55
	sta WSYNC
	SOUND
	ldx #0
	stx ENAM0
	stx ENAM1
	stx PF0
	stx PF1
	stx PF2
	rts

; ---- Text48: THE GAME, 48 pixels wide. Players 0 and 1 have three copies
; each, 16 pixels apart, and P1 starts 8 pixels after P0: six columns of 8.
; With VDELP0 and VDELP1 each player shows the value its register had when the
; other one was last written, so the line writes the first three columns
; early and the last three just as the beam reaches them (cycles 44, 47, 50
; and 53). The 48-pixel technique of Activision's six-digit scores.
Text48 SUBROUTINE
	ldy #TEXT48_LINES
	sty Temp2
.line
	sta WSYNC
	lda #AMPLITUDE          ; 2
	sta AUDV0               ; 5
	lda #STREAM_THE_GAME    ; 7
	sta GRP0                ; 10  column 1 (waits in the delayed GRP0)
	lda #STREAM_THE_GAME+1  ; 12
	sta GRP1                ; 15  column 2; column 1 shows
	lda #STREAM_THE_GAME+2  ; 17
	sta GRP0                ; 20  column 3; column 2 shows
	lda #STREAM_THE_GAME+3  ; 22
	sta Temp                ; 25
	lda #STREAM_THE_GAME+4  ; 27
	tax                     ; 29
	lda #STREAM_THE_GAME+5  ; 31
	tay                     ; 33
	lda Temp                ; 36
	nop                     ; 38
	bit Temp                ; 41
	sta GRP1                ; 44  column 4; column 3 shows
	stx GRP0                ; 47  column 5; column 4 shows
	sty GRP1                ; 50  column 6; column 5 shows
	sta GRP0                ; 53  column 6 shows
	dec Temp2               ; 58
	bne .line               ; 61
	sta WSYNC
	SOUND
	ldx #0
	stx GRP0
	stx GRP1
	stx GRP0
	rts

; ---- GameKernel: 192 lines: the score (16) and the room (176) ---------------
GameKernel SUBROUTINE
	jsr BeamOn
	ldx #3
	jsr BlankLines
	ldx #10                 ; the score in score mode (CTRLPF from the C code):
.digit
	sta WSYNC               ; the left PF1 in COLUP0, the right one in COLUP1
	lda #AMPLITUDE          ; 2
	sta AUDV0               ; 5
	lda #STREAM_SCORE       ; 7
	sta PF1                 ; 10  the left PF1: the score
	jsr Wait12              ;   22
	jsr Wait12              ;   34
	nop                     ;   36
	nop                     ;   38
	lda #STREAM_LIVES       ; 2 40
	nop                     ;   42
	sta PF1                 ; 3 45  the right PF1 (cycles 39 to 54): the lives
	dex
	bne .digit
	sta WSYNC
	SOUND
	ldx #0
	stx PF1
	lda #DSCOMM             ; the room's CTRLPF (mirrored, a ball 4 clocks wide)
	sta CTRLPF
	lda #DSCOMM             ; the shown enemy's colour
	sta COLUP1
	sta WSYNC
	SOUND

; Two lines per pass (ROOM_PAIRS passes): the first writes the playfield, the
; second the sprites and the shot.
	ldx #ROOM_PAIRS
.pair
	sta WSYNC
	lda #AMPLITUDE          ; 2
	sta AUDV0               ; 5
	lda #STREAM_ROOM        ; 7
	sta PF0                 ; 10
	lda #STREAM_ROOM+1      ; 12
	sta PF1                 ; 15
	lda #STREAM_ROOM+2      ; 17
	sta PF2                 ; 20
	sta WSYNC
	lda #AMPLITUDE          ; 2
	sta AUDV0               ; 5
	lda #STREAM_HERO        ; 7
	sta GRP0                ; 10
	lda #STREAM_HERO_COLOUR ; 12
	sta COLUP0              ; 15
	lda #STREAM_ENEMY       ; 17
	sta GRP1                ; 20
	lda #STREAM_SHOT        ; 22
	sta ENABL               ; 25
	dex                     ; 27
	bne .pair               ; 30

	sta WSYNC
	SOUND
	ldx #0
	stx GRP0
	stx GRP1
	stx ENABL
	stx PF0
	stx PF1
	stx PF2
	rts

; ============================================================================
; Shared routines
; ============================================================================

; ---- BeamOn: the first visible line.
BeamOn
	sta WSYNC
	SOUND
	ldx #0
	stx VBLANK              ; beam on
	rts

; ---- BlankLines: X = lines with nothing drawn, and the sound on each.
BlankLines SUBROUTINE
.line
	sta WSYNC
	SOUND
	dex
	bne .line
	rts

; ---- Wait12: jsr and rts take 12 cycles: a short wait in a kernel.
Wait12
	rts

; ---- PosObject: A = x (0-159), X = object (0 P0, 1 P1, 2 M0, 3 M1, 4 BL).
; Ends with HMxx set: an HMOVE afterwards puts it in place.
PosObject SUBROUTINE
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
	SOUND                   ; after RESxx, which needs the line's start
	rts

	ORG $7FF0
	RORG $FFF0
	ds.b 12, 0              ; the cartridge's registers
	.word Reset, Reset
