; ---- The other banks: {{FORMAT}}, {{BANKS}} banks of 4K -------------------------------
; The 2600 sees 4K of the cartridge at a time, at $F000-$FFFF; touching
; BANK0_HOTSPOT+n (reading or writing it) shows bank n. The game is all in
; bank 0; banks 1 on are empty, for the game to grow into (the empty F8, F6
; and F4 template shows how to call code in another bank).
; The cartridge can start in any bank: each one ends with the same code at
; $FFE0, which switches to bank 0 and jumps to Reset.
BANKS         = {{BANKS}}
BANK0_HOTSPOT = {{HOTSPOT}}     ; bank 0; bank 1 is BANK0_HOTSPOT+1, and so on

        MAC BANK_END            ; {1}: the bank's number
        IF . > $FFE0
          ECHO "Bank", [{1}]d, "is full: its code runs into $FFE0"
          ERR
        ENDIF
        ORG [{1}*$1000]+$FE0
        RORG $FFE0
        sta BANK0_HOTSPOT       ; $FFE0: power on, in any bank: bank 0, which
        jmp Reset               ; $FFE3: goes on here
        ORG [{1}*$1000]+$FFC
        RORG $FFFC
        .word $FFE0             ; RESET
        .word $FFE0             ; IRQ/BRK
        ENDM

        BANK_END 0
BANK    SET 1
        REPEAT BANKS-1
        ORG BANK*$1000
        RORG $F000
        BANK_END BANK
BANK    SET BANK+1
        REPEND
