; SPDX-License-Identifier: CC0-1.0 (VCSIDE ACE proof of concept)
; Colour bars for the 6507: a plain 4K ROM that the ACE program serves.
        processor 6502
VSYNC   = $00
VBLANK  = $01
WSYNC   = $02
COLUBK  = $09
        seg code
        org $F000
start:  sei
        cld
        ldx #$FF
        txs
frame:  lda #2
        sta VBLANK
        sta VSYNC
        sta WSYNC
        sta WSYNC
        sta WSYNC
        lda #0
        sta VSYNC
        ldx #37
vblank: sta WSYNC
        dex
        bne vblank
        stx VBLANK
        ldy #192
lines:  sty COLUBK
        sta WSYNC
        dey
        bne lines
        lda #2
        sta VBLANK
        ldx #30
over:   sta WSYNC
        dex
        bne over
        jmp frame
        org $FFFC
        .word start
        .word start
