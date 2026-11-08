; hardware.h - Atari 2600 TIA and RIOT registers for DASM (VCSIDE template).
; SPDX-License-Identifier: CC0-1.0  (public domain: use it in any game)
; Same names as the classic vcs.h. Reads use the $30-$3D mirror of the TIA.

        IFNCONST TIA_BASE_ADDRESS
TIA_BASE_ADDRESS = 0
        ENDIF
        IFNCONST TIA_BASE_READ_ADDRESS
TIA_BASE_READ_ADDRESS = TIA_BASE_ADDRESS
        ENDIF

        SEG.U TIA_WRITE
        ORG TIA_BASE_ADDRESS
VSYNC   ds 1    ; $00 vertical sync (D1)
VBLANK  ds 1    ; $01 vertical blank (D1), input latch (D6), dump paddles (D7)
WSYNC   ds 1    ; $02 wait for the end of the scanline (strobe)
RSYNC   ds 1    ; $03 reset horizontal sync (strobe, testing only)
NUSIZ0  ds 1    ; $04 number/size of player 0 and missile 0
NUSIZ1  ds 1    ; $05
COLUP0  ds 1    ; $06 colour of player 0 and missile 0
COLUP1  ds 1    ; $07
COLUPF  ds 1    ; $08 colour of playfield and ball
COLUBK  ds 1    ; $09 background colour
CTRLPF  ds 1    ; $0A playfield control: reflect, score, priority, ball size
REFP0   ds 1    ; $0B reflect player 0 (D3)
REFP1   ds 1    ; $0C
PF0     ds 1    ; $0D playfield register 0 (D4-D7)
PF1     ds 1    ; $0E
PF2     ds 1    ; $0F
RESP0   ds 1    ; $10 reset player 0 position (strobe)
RESP1   ds 1    ; $11
RESM0   ds 1    ; $12
RESM1   ds 1    ; $13
RESBL   ds 1    ; $14
AUDC0   ds 1    ; $15 audio control
AUDC1   ds 1    ; $16
AUDF0   ds 1    ; $17 audio frequency
AUDF1   ds 1    ; $18
AUDV0   ds 1    ; $19 audio volume
AUDV1   ds 1    ; $1A
GRP0    ds 1    ; $1B graphics player 0
GRP1    ds 1    ; $1C
ENAM0   ds 1    ; $1D enable missile 0 (D1)
ENAM1   ds 1    ; $1E
ENABL   ds 1    ; $1F enable ball (D1)
HMP0    ds 1    ; $20 horizontal motion player 0 (D4-D7)
HMP1    ds 1    ; $21
HMM0    ds 1    ; $22
HMM1    ds 1    ; $23
HMBL    ds 1    ; $24
VDELP0  ds 1    ; $25 vertical delay player 0 (D0)
VDELP1  ds 1    ; $26
VDELBL  ds 1    ; $27
RESMP0  ds 1    ; $28 lock missile 0 to player 0 (D1)
RESMP1  ds 1    ; $29
HMOVE   ds 1    ; $2A apply horizontal motion (strobe, right after WSYNC)
HMCLR   ds 1    ; $2B clear horizontal motion registers (strobe)
CXCLR   ds 1    ; $2C clear collision latches (strobe)

        SEG.U TIA_READ
        ORG TIA_BASE_READ_ADDRESS
CXM0P   ds 1    ; $00 collisions M0-P1 (D7), M0-P0 (D6)
CXM1P   ds 1    ; $01 M1-P0, M1-P1
CXP0FB  ds 1    ; $02 P0-PF, P0-BL
CXP1FB  ds 1    ; $03 P1-PF, P1-BL
CXM0FB  ds 1    ; $04 M0-PF, M0-BL
CXM1FB  ds 1    ; $05 M1-PF, M1-BL
CXBLPF  ds 1    ; $06 BL-PF (D7)
CXPPMM  ds 1    ; $07 P0-P1, M0-M1
INPT0   ds 1    ; $08 paddle 0 (D7)
INPT1   ds 1    ; $09
INPT2   ds 1    ; $0A
INPT3   ds 1    ; $0B
INPT4   ds 1    ; $0C left fire button (D7, 0 = pressed)
INPT5   ds 1    ; $0D right fire button

        SEG.U RIOT
        ORG $280
SWCHA   ds 1    ; $280 joysticks: P0 right/left/down/up in D7-D4, P1 in D3-D0 (0 = pressed)
SWACNT  ds 1    ; $281 port A direction
SWCHB   ds 1    ; $282 console switches: D0 reset, D1 select, D3 colour/BW, D6/D7 difficulty
SWBCNT  ds 1    ; $283 port B direction
INTIM   ds 1    ; $284 timer value
TIMINT  ds 1    ; $285 timer interrupt flag (D7)

        ORG $294
TIM1T   ds 1    ; $294 set timer, 1 cycle per tick
TIM8T   ds 1    ; $295 8 cycles per tick
TIM64T  ds 1    ; $296 64 cycles per tick
T1024T  ds 1    ; $297 1024 cycles per tick
