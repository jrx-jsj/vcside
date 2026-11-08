; {{NAME}} - made with VCSIDE
; 4K cartridge, {{TV}} timing. Build: F7 (or Build > Build). Run: F5.

        processor 6502
        include "hardware.h"

; ---- TV timing (scanlines) -------------------------------------------------
{{TVDEFS}}

; ---- RAM -------------------------------------------------------------------
        SEG.U Variables
        ORG $80
Frame   ds 1

; ---- Code ------------------------------------------------------------------
        SEG Code
        ORG $F000

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
        inc Frame
.vblank lda INTIM
        bne .vblank
        sta WSYNC
        sta VBLANK              ; A = 0: beam on

        ldx #VISIBLE_LINES
        lda Frame
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

        ORG $FFFC
        .word Reset
        .word Reset
