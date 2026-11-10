; {{NAME}} - made with VCSIDE
; Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
; CDFJ cartridge (32K), {{TV}} timing. Build: F7 (runs make). Run: F5.
;
; Example: a game whose logic is C run by the cartridge (game.c), and music the cartridge
; plays. A CDFJ cartridge (Harmony/Melody) has an ARM that runs the C code when the 6507
; writes CALLFN, and data streams: "lda #stream" reads the next byte of one, in 2 cycles.
; The ARM plays three voices with 32-sample waveforms and adds them up; the 6507 only
; copies that sum (the AMPLITUDE stream) to AUDV0 on every line, with AUDC0 = 0.
;
; The joystick moves the square; the fire button plays the "fire" effect and reaching
; an edge plays "bump" (game.c). The background shows the sound: each line's
; luminance is the amplitude written to AUDV0 on it.
;
; With fast fetch on, "lda #n" with n from 0 to $23 reads data stream n instead of
; loading n: this program loads its constants with ldx # and ldy #.
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
HERO_STREAM    = 0              ; the square's graphics, a byte per line (game.c points it)
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
Temp    ds 1

; ---- The ARM driver and the C code -------------------------------------------
        SEG Code
        ORG $0000
        incbin "cdfj_driver.bin"
        ORG $0800
        incbin "build/arm.bin"

; ---- Bank 6: the 6507's program ----------------------------------------------
        ORG $7000
        RORG $F000

Reset   sei
        cld
        ldx #0
        txa
.clear  dex
        txs
        pha
        bne .clear              ; clears RAM and TIA, S ends at $FF
        ldx #$F0
        stx SETMODE             ; fast fetch on, the cartridge plays voices
        ldx #$1E                ; yellow square
        stx COLUP0
        ldy #FUNCTION_START
        jsr CallArm             ; the song from its start (AUDC0 is 0 already)

StartFrame
        ldx #2
        stx VBLANK
        stx VSYNC
        ldy #3
.vsync  sta WSYNC
        SOUND                   ; the sound goes on in every line
        dey
        bne .vsync
        sty VSYNC
        ldx #(VBLANK_LINES*76-14)/64  ; timer for the vertical blank
        stx TIM64T

        ldy #FUNCTION_FRAME
        jsr CallArm             ; the frame: joystick, square, effects and music
        SOUND
        ldx #>LAYOUT_HERO_X     ; what the C code left: the square's x
        stx DSPTR
        ldx #<LAYOUT_HERO_X
        stx DSPTR
        lda #DSCOMM
        ldx #0                  ; player 0
        jsr PosObject
        sta WSYNC
        sta HMOVE
        SOUND
.vblank sta WSYNC               ; until the timer runs out, a line at a time
        SOUND
        lda INTIM
        bne .vblank
        sta VBLANK              ; A = 0: beam on

; ---- Kernel: one line per pass ------------------------------------------------
        ldy #VISIBLE_LINES
.line   sta WSYNC
        lda #AMPLITUDE          ; 2
        sta AUDV0               ; 3
        asl                     ; 2  the amplitude (0-15) as luminance
        and #$0E                ; 2
        ora #$80                ; 2  in blue
        sta COLUBK              ; 3
        lda #HERO_STREAM        ; 2  this line of the square
        sta GRP0                ; 3
        dey                     ; 2
        bne .line               ; 3

        sta WSYNC
        SOUND
        ldx #0
        stx COLUBK
        stx GRP0
        ldx #2
        stx VBLANK
        ldx #(OVERSCAN_LINES*76-14)/64
        stx TIM64T
.over   sta WSYNC
        SOUND
        lda INTIM
        bne .over
        jmp StartFrame

; ---- CallArm: Y = what to do (FUNCTION_x). The joystick, the button and the
; number of lines go to the display data, then the C code runs (game_arm).
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
        ldx #VISIBLE_LINES
        stx DSWRITE             ; LAYOUT_LINES
        ldx #$FE
        stx CALLFN              ; the 6507 waits here while the ARM runs
        rts

; ---- PosObject: A = x (0-159), X = object (0 = P0). Ends with HMxx set. ------
PosObject
        sta WSYNC
        sec
.div    sbc #15
        bcs .div
        eor #7
        asl
        asl
        asl
        asl
        sta HMP0,x
        sta RESP0,x
        rts

        ORG $7FF0
        RORG $FFF0
        ds.b 12, 0              ; the cartridge's registers
        .word Reset, Reset
