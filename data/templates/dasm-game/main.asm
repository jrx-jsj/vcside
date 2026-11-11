; {{NAME}} - made with VCSIDE
; Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
; 4K cartridge. The TV system is chosen in the title menu (it starts as {{TV}}).
; Build: F7 (or Build > Build). Run: F5.
;
; Example: a small game. A title screen with a menu (PLAY, and the TV system),
; then a room with walls where the hero walks and shoots at two enemies.
; Hitting one scores a point; touching one costs a life; with no lives left the
; game goes back to the title. RESET also goes back to the title.
;
; Graphics (open them from the project panel; saving one writes its .gfx.asm):
;   hero.vcsgfx   the hero (P0) and its animations, idle and walk
;   enemy.vcsgfx  the enemy (P1), one colour (its first row's)
;   room.vcsgfx   the room's playfield: 22 rows of 8 lines, mirrored
;   title.vcsgfx  the title's VCSIDE and the menu's texts, as playfields
; THE GAME is written in main.asm (TheGame0-5), six columns of 8 pixels.
; Music and sound: theme.vcsmusic (saving it writes theme.music.asm) is the song,
; played on both screens, and three effects: shot, hit and lose.
;
; How it works:
; - The frame: VSYNC, the vertical blank (the game logic, timed by the RIOT),
;   192 visible lines, and the overscan (the collisions). PAL has 25 more blank
;   lines above and below the same 192 (312 in all); NTSC and PAL60 have 262.
;   The colours are written for NTSC and turned into PAL ones (ConvertColour).
; - The game's kernel draws two lines per pass: the first writes the sprites
;   and the shot, worked out on the pass before; the second the playfield.
; - The two enemies take turns on player 1, one per frame (flicker).
; - The TIA says what touched what (collision registers): the hero and an
;   enemy go back where they were when they touch a wall, the shot ends there.

        processor 6502
        include "2600.h"

; ---- Settings ----------------------------------------------------------------
DEFAULT_SYSTEM = {{TV_SYSTEM}}  ; the menu's first TV system: 0 NTSC, 1 PAL60, 2 PAL
LIVES          = 3
ROOM_PAIRS     = room_ROWS * room_LINES / 2  ; 88 line pairs of room
HERO_START_X   = 76
HERO_START_Y   = 8              ; line pair of the hero's bottom row (1 = the bottom one)
SHOT_SPEED     = 3

; ---- RAM -------------------------------------------------------------------
        SEG.U Variables
        ORG $80
System    ds 1          ; 0 NTSC, 1 PAL60, 2 PAL
Mode      ds 1          ; 0 the title screen, 1 the game
MenuRow   ds 1          ; 0 PLAY, 1 TV
LastStick ds 1          ; SWCHA on the last frame (0 bits: directions held)
Button    ds 1          ; the fire button on the last frame (bit 7: 0 down)
Frame     ds 1
Random    ds 1          ; a new pseudo-random number every frame (never 0)
HeroX     ds 1          ; colour clocks from the left
HeroY     ds 1          ; line pair of the hero's bottom row, from the bottom
OldHeroX  ds 1          ; where it was before this frame's move
OldHeroY  ds 1
Facing    ds 1          ; REFP0: 0 right, 8 left
Anim      ds 1          ; 0 idle, 1 walk (the order of the tables below)
Step      ds 1          ; step of the animation
Timer     ds 1          ; frames into the step
GfxPtr    ds 2          ; the hero's frame
ColPtr    ds 2          ; its colours (HeroColours)
EnemyX    ds 2          ; two enemies
EnemyY    ds 2
EnemyDX   ds 2          ; +1 or -1 ($FF)
EnemyDY   ds 2
EnemyLive ds 1          ; bit 0: enemy 0, bit 1: enemy 1
Shown     ds 1          ; the enemy drawn on this frame (0 or 1)
OldEnemyX ds 1          ; where the shown enemy was before its move
OldEnemyY ds 1
EnemyRow  ds 1          ; line pair of the shown enemy's bottom row ($F0: none)
EnemyPtr  ds 2          ; its frame
ShotX     ds 1
ShotPair  ds 1          ; the line pair the shot is on (0: no shot)
ShotDX    ds 1          ; SHOT_SPEED or -SHOT_SPEED
Score     ds 1          ; BCD, 00-99
Lives     ds 1
CurGfx0   ds 1          ; what the kernel's next pass writes
CurCol0   ds 1
CurGfx1   ds 1
CurShot   ds 1
HeroColours ds hero_P0_HEIGHT   ; the hero frame's colours, for the TV system
ScoreRows ds 5          ; the score's two digits, PF1, top row first
LivesRows ds 5          ; the lives' digit
Temp      ds 2          ; scratch: pointers and the score's digits
Temp2     ds 1
EnemyColour ds 1        ; COLUP1 for the shown enemy, for the TV system
LogoColour  ds 1        ; the title's colours, and the lives' in the game,
TextColour  ds 1        ; worked out in the vertical blank (not between WSYNCs)
ColourKeep  ds 1        ; ConvertColour's own two bytes
ColourLum   ds 1
MusicRam  ds 19         ; the music player's
MUSIC_RAM = MusicRam

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
        lda #DEFAULT_SYSTEM
        sta System
        lda #$80
        sta Button              ; as if up: holding it at power on does nothing
        lda #$FF
        sta LastStick
        sta Random              ; any start but 0
        jsr MusicInit           ; the song from its start

StartFrame SUBROUTINE
        lda #2
        sta WSYNC               ; VSYNC from the start of a line
        sta VBLANK
        sta VSYNC
        sta WSYNC
        sta WSYNC
        sta WSYNC
        lda #0
        sta VSYNC
        ldx System
        lda VblankTime,x        ; the vertical blank's length for the TV system
        sta TIM64T
        inc Frame
        lda Random              ; an 8-bit Galois LFSR: shift, and when a 1
        lsr                     ; falls out, flip the taps ($B4: all 255
        bcc .sameTaps           ; values but 0 before it repeats)
        eor #$B4
.sameTaps
        sta Random

        jsr MusicFrame          ; the music's writes for this frame
        lda Mode
        bne .game
        jsr TitleLogic
        jmp .wait
.game   jsr GameLogic
.wait   lda INTIM
        bne .wait
        sta WSYNC
        sta VBLANK              ; A = 0: beam on

        lda Mode
        bne .drawGame
        jsr TitleKernel
        jmp .overscan
.drawGame
        jsr GameKernel

.overscan
        lda #2
        sta VBLANK
        sta WSYNC               ; the timer from the start of a line: always the same length
        ldx System
        lda OverscanTime,x
        sta TIM64T
        lda Mode
        beq .animate
        jsr Collisions
.animate                        ; the hero for the next frame: with PAL colours it
        jsr Animate             ; does not fit in the vertical blank (and on the
.over   lda INTIM               ; title too, so it is ready when the game starts)
        bne .over
        jmp StartFrame

; Timer values (TIM64T, 64 cycles each) for the blank lines of each TV system,
; measured in Gopher2600 for 262 lines (NTSC, PAL60) and 312 (PAL: 25 more
; above and below). A timer that runs out before the program waits for it
; makes the frame longer: keep the vertical blank's work inside its time.
VblankTime      .byte 43, 43, 73
OverscanTime    .byte 36, 36, 65

; ============================================================================
; Title screen
; ============================================================================

; ---- TitleLogic: the menu. Up and down choose the row, left and right change
; the TV system; the button on PLAY starts the game, on TV it changes the system.
TitleLogic SUBROUTINE
        jsr StickPresses        ; A: directions pressed on this frame (1 bits)
        sta Temp
        and #$30                ; up or down: the other row
        beq .noRow
        lda MenuRow
        eor #1
        sta MenuRow
.noRow  lda MenuRow
        beq .play
        lda Temp
        and #$80                ; right
        beq .noRight
        jsr NextSystem
.noRight
        lda Temp
        and #$40                ; left
        beq .buttonTv
        dec System
        bpl .buttonTv
        lda #2
        sta System
.buttonTv
        jsr ButtonPress
        bcc .place
        jsr NextSystem
        jmp .place
.play   jsr ButtonPress
        bcc .place
        jmp StartGame
.place  lda #logo_COLUPF        ; the colours for the TV system
        jsr ConvertColour
        sta LogoColour
        lda #$9C                ; THE GAME: light blue
        jsr ConvertColour
        sta TextColour
        ; THE GAME: players 0 and 1, three close copies each, 8 pixels apart
        lda #$03
        sta NUSIZ0
        sta NUSIZ1
        lda #1
        sta VDELP0
        sta VDELP1
        lda #TEXT48_X
        ldx #0
        jsr PosObject
        lda #TEXT48_X+8
        ldx #1
        jsr PosObject
        sta WSYNC
        sta HMOVE
        rts

NextSystem SUBROUTINE
        inc System
        lda System
        cmp #3
        bcc .ok
        lda #0
        sta System
.ok     rts

; ---- TitleKernel: 192 lines: VCSIDE (playfield), THE GAME (48 pixels of
; players), PLAY and the TV system. The selected row is white, the other grey.
TitleKernel SUBROUTINE
        lda #0
        sta ENAM0
        sta CTRLPF              ; the texts are not mirrored
        sta COLUBK
        ldx #24
        jsr BlankLines
        lda LogoColour
        sta COLUPF
        DRAW_TEXT logo
        ldx #8
        jsr BlankLines
        lda TextColour
        sta COLUP0
        sta COLUP1
        jsr Text48
        ldx #40
        jsr BlankLines
        ldy #$0E                ; white when selected
        lda MenuRow
        beq .playColour
        ldy #$06
.playColour
        sty COLUPF
        DRAW_TEXT play
        ldx #10
        jsr BlankLines
        ldy #$0E
        lda MenuRow
        bne .tvColour
        ldy #$06
.tvColour
        sty COLUPF
        lda System
        beq .ntsc
        cmp #1
        beq .pal60
        DRAW_TEXT pal
        jmp .rest
.ntsc   DRAW_TEXT ntsc
        jmp .rest
.pal60  DRAW_TEXT pal60
.rest   ldx #192-24-[logo_ROWS*logo_LINES+1]-8-[TEXT48_LINES+1]-40-[play_ROWS*play_LINES+1]-10-[ntsc_ROWS*ntsc_LINES+1]
        jmp BlankLines

; ---- Text48: THE GAME, 48 pixels wide. Players 0 and 1 have three copies
; each, 16 pixels apart, and P1 starts 8 pixels after P0: six columns of 8.
; With VDELP0 and VDELP1 each player shows the value its register had when the
; other one was last written, so the line writes the first three columns
; early and the last three just as the beam reaches them (cycles 45, 48, 51
; and 54). The 48-pixel technique of Activision's six-digit scores.
TEXT48_X     = 56               ; P0's place: the text from x = 56 to 103
TEXT48_LINES = 14
Text48 SUBROUTINE
        lda #TEXT48_LINES-1
        sta Temp2               ; the line, from the top (the tables are bottom first)
.line   sta WSYNC
        ldy Temp2               ; 3  3
        lda TheGame0,y          ; 4  7
        sta GRP0                ; 3 10  column 1 (waits in the delayed GRP0)
        lda TheGame1,y          ; 4 14
        sta GRP1                ; 3 17  column 2; column 1 shows
        lda TheGame2,y          ; 4 21
        sta GRP0                ; 3 24  column 3; column 2 shows
        lda TheGame3,y          ; 4 28
        sta Temp                ; 3 31
        ldx TheGame4,y          ; 4 35
        lda TheGame5,y          ; 4 39
        ldy Temp                ; 3 42
        sty GRP1                ; 3 45  column 4; column 3 shows
        stx GRP0                ; 3 48  column 5; column 4 shows
        sta GRP1                ; 3 51  column 6; column 5 shows
        sta GRP0                ; 3 54  column 6 shows
        dec Temp2               ; 5 59
        bpl .line               ; 3 62
        sta WSYNC
        lda #0
        sta GRP0
        sta GRP1
        sta GRP0
        rts

; DRAW_TEXT name: draws a playfield of title.vcsgfx, its rows top first. Its
; rows are 40 columns (asymmetric): each line writes PF0-PF2 for the left half
; and again, in time, for the right one.
        MAC DRAW_TEXT
        ldy #{1}_ROWS-1         ; the tables are bottom row first
.row    ldx #{1}_LINES
.line   sta WSYNC
        lda {1}_PF0,y           ; 4
        sta PF0                 ; 3   7
        lda {1}_PF1,y           ; 4  11
        sta PF1                 ; 3  14
        lda {1}_PF2,y           ; 4  18
        sta PF2                 ; 3  21
        lda {1}_PF0R,y          ; 4  25
        sta PF0                 ; 3  28  the right PF0: from cycle 28 to 49
        lda {1}_PF1R,y          ; 4  32
        nop                     ; 2  34
        nop                     ; 2  36
        sta PF1                 ; 3  39  the right PF1: from cycle 39 to 54
        lda {1}_PF2R,y          ; 4  43
        nop                     ; 2  45
        nop                     ; 2  47
        sta PF2                 ; 3  50  the right PF2: from cycle 50 to 59
        dex
        bne .line
        dey
        bpl .row
        sta WSYNC
        lda #0
        sta PF0
        sta PF1
        sta PF2
        ENDM

; ============================================================================
; The game
; ============================================================================

StartGame SUBROUTINE
        lda #1
        sta Mode
        lda #LIVES
        sta Lives
        lda #0
        sta Score
        sta ShotPair
        jsr PlaceHero
        jmp NewWave

PlaceHero SUBROUTINE
        lda #HERO_START_X
        sta HeroX
        sta OldHeroX
        lda #HERO_START_Y
        sta HeroY
        sta OldHeroY
        rts

; Both enemies back, near the top of the room.
NewWave SUBROUTINE
        ldx #1
.enemy  lda WaveX,x
        sta EnemyX,x
        lda #70
        sta EnemyY,x
        lda WaveDX,x
        sta EnemyDX,x
        lda #$FF                ; down
        sta EnemyDY,x
        dex
        bpl .enemy
        lda #3
        sta EnemyLive
        rts
WaveX   .byte 40, 112
WaveDX  .byte 1, $FF

; ---- GameLogic: one frame of the game, in the vertical blank ----------------
GameLogic SUBROUTINE
        lda SWCHB
        lsr                     ; D0 = 0: RESET is down
        bcs .noReset
        lda #0
        sta Mode
        rts
.noReset
        jsr MoveHero
        jsr Shoot
        jsr MoveEnemy
        jsr PrepareScore
        ; the colours for the TV system
        lda #room_COLUPF
        jsr ConvertColour
        sta COLUPF
        lda #room_COLUBK
        jsr ConvertColour
        sta COLUBK
        lda enemy_P1_Colour0+enemy_P1_HEIGHT-1  ; the enemy's top row's colour
        ldx Shown
        beq .first
        clc
        adc #$40                ; the second enemy: another hue
.first  jsr ConvertColour
        sta EnemyColour         ; COLUP1, written after the score
        lda #$1C                ; the lives: yellow
        jsr ConvertColour
        sta TextColour
        ; the objects' places: P0, P1 and M0
        lda Facing
        sta REFP0
        lda #$20                ; missile 0 four clocks wide, one copy of each player
        sta NUSIZ0
        lda #0
        sta NUSIZ1
        sta VDELP0              ; (the title's text uses them)
        sta VDELP1
        lda HeroX
        ldx #0
        jsr PosObject
        ldx Shown
        lda EnemyX,x
        ldx #1
        jsr PosObject
        lda ShotX
        ldx #2
        jsr PosObject
        sta WSYNC
        sta HMOVE
        rts

; ---- MoveHero: the joystick moves the hero one clock (or one line pair) a
; frame and chooses its animation and the way it faces.
MoveHero SUBROUTINE
        lda HeroX
        sta OldHeroX
        lda HeroY
        sta OldHeroY
        ldy #0                  ; idle unless a direction is held
        lda SWCHA
        bmi .noRight            ; D7 = 0: right
        ldy #1
        lda #0
        sta Facing
        lda HeroX
        cmp #151
        bcs .noRight
        inc HeroX
.noRight
        lda SWCHA
        asl
        bmi .noLeft             ; D6 = 0: left
        ldy #1
        lda #8
        sta Facing
        lda HeroX
        cmp #2
        bcc .noLeft
        dec HeroX
.noLeft lda SWCHA
        and #$10                ; D4 = 0: up
        bne .noUp
        ldy #1
        lda HeroY
        cmp #ROOM_PAIRS-hero_P0_HEIGHT
        bcs .noUp
        inc HeroY
.noUp   lda SWCHA
        and #$20                ; D5 = 0: down
        bne .noDown
        ldy #1
        lda HeroY
        cmp #2
        bcc .noDown
        dec HeroY
.noDown cpy Anim
        beq .same
        sty Anim                ; another animation: from its first step
        lda #0
        sta Step
        sta Timer
.same   rts

; ---- Shoot: the button fires one shot the way the hero faces; it flies until
; it hits something or leaves the screen.
Shoot SUBROUTINE
        jsr ButtonPress
        bcc .move
        lda ShotPair
        bne .move               ; one shot at a time
        lda HeroY
        clc
        adc #hero_P0_HEIGHT/2
        sta ShotPair
        ldx #SHOT_SPEED
        lda HeroX
        clc
        adc #8                  ; from the hero's right side...
        ldy Facing
        beq .from
        sec
        sbc #10                 ; ...or its left one
        ldx #256-SHOT_SPEED
.from   sta ShotX
        stx ShotDX
        lda #MUSIC_EFFECT_SHOT
        jsr MusicEffect
.move   lda ShotPair
        beq .done
        lda ShotX
        clc
        adc ShotDX
        sta ShotX
        cmp #158                ; off the screen (on the left it wraps above 158)
        bcc .done
        lda #0
        sta ShotPair
.done   rts

; ---- MoveEnemy: this frame's enemy (they take turns) moves diagonally; the
; collisions turn it round.
MoveEnemy SUBROUTINE
        lda Frame
        and #1
        sta Shown
        tax
        lda #$F0                ; not drawn
        sta EnemyRow
        lda EnemyLive
        and EnemyBit,x
        beq .anim
        lda EnemyX,x
        sta OldEnemyX
        clc
        adc EnemyDX,x
        sta EnemyX,x
        lda EnemyY,x
        sta OldEnemyY
        clc
        adc EnemyDY,x
        sta EnemyY,x
        sta EnemyRow
.anim   lda Frame
        lsr
        lsr
        lsr
        lsr
        and #1                  ; its two frames, 16 frames each
        tay
        lda enemy_P1_FramePtrLo,y
        sta EnemyPtr
        lda enemy_P1_FramePtrHi,y
        sta EnemyPtr+1
        rts
EnemyBit .byte 1, 2

; ---- Collisions: what touched what on the frame just drawn (the overscan) ---
Collisions SUBROUTINE
        bit CXP0FB              ; D7: the hero and the playfield
        bpl .heroFree
        lda OldHeroX
        sta HeroX
        lda OldHeroY
        sta HeroY
.heroFree
        ldx Shown
        bit CXP1FB              ; D7: the enemy and the playfield
        bpl .enemyFree
        lda OldEnemyX           ; back where it was...
        sta EnemyX,x
        lda OldEnemyY
        sta EnemyY,x
        lda Random              ; ...and the other way on one axis. The TIA does not
        and #1                  ; say which side it touched, so the axis is chosen at
        beq .turnY              ; random; a wrong guess touches again and turns the
        lda #0                  ; other one (a fixed rule traps them in a loop).
        sec
        sbc EnemyDX,x
        sta EnemyDX,x
        jmp .enemyFree
.turnY  lda #0
        sec
        sbc EnemyDY,x
        sta EnemyDY,x
.enemyFree
        bit CXM0FB              ; D7: the shot and the playfield
        bpl .shotFree
        lda #0
        sta ShotPair
.shotFree
        bit CXM0P               ; D7: the shot and the enemy
        bpl .noHit
        lda ShotPair
        beq .noHit
        lda #0
        sta ShotPair
        lda EnemyBit,x
        eor #$FF
        and EnemyLive
        sta EnemyLive
        sed
        lda Score
        clc
        adc #1
        sta Score
        cld
        lda #MUSIC_EFFECT_HIT
        jsr MusicEffect
        lda EnemyLive
        bne .noHit
        jsr NewWave             ; both gone: two more
.noHit  bit CXPPMM              ; D7: the hero and the enemy
        bpl .done
        lda #MUSIC_EFFECT_LOSE
        jsr MusicEffect
        jsr PlaceHero
        jsr NewWave             ; the enemies back at the top, away from the hero
        dec Lives
        bne .done
        lda #0                  ; no lives left: the title
        sta Mode
.done   sta CXCLR
        rts

; ---- GameKernel: 192 lines: the score (16) and the room (176) ---------------
GameKernel SUBROUTINE
        ; the score: the left half in COLUP0, the right one in COLUP1 (score mode)
        lda #$02
        sta CTRLPF
        lda #$0E
        sta COLUP0
        lda TextColour          ; the lives' colour
        sta COLUP1
        ldx #3
        jsr BlankLines
        ldy #0
.digit  ldx #2                  ; two lines per row
.digitLine
        sta WSYNC
        lda ScoreRows,y         ; 4
        sta PF1                 ; 3  7  the left PF1: the score
        jsr Wait12              ;   19
        jsr Wait12              ;   31
        nop                     ;   33
        nop                     ;   35
        nop                     ;   37
        lda LivesRows,y         ; 4 41
        sta PF1                 ; 3 44  the right PF1 (cycles 39 to 54): the lives
        dex
        bne .digitLine
        iny
        cpy #5
        bne .digit
        sta WSYNC
        lda #0
        sta PF1
        ; the room: mirrored, and the enemy's colour
        lda #room_CTRLPF
        sta CTRLPF
        lda EnemyColour
        sta COLUP1
        lda #0
        sta CurGfx0
        sta CurGfx1
        sta CurShot
        sta WSYNC
        ldy #room_ROWS-1        ; the top row's playfield for the first line
        lda room_PF0,y
        sta PF0
        lda room_PF1,y
        sta PF1
        lda room_PF2,y
        sta PF2

; Two lines per pass, X = line pairs left (ROOM_PAIRS down to 1).
        ldx #ROOM_PAIRS
.pair   sta WSYNC
        lda CurGfx0             ; 3
        sta GRP0                ; 3  6
        lda CurCol0             ; 3  9
        sta COLUP0              ; 3 12
        lda CurGfx1             ; 3 15
        sta GRP1                ; 3 18
        lda CurShot             ; 3 21
        sta ENAM0               ; 3 24
        txa                     ; 2 26  the hero's row on the next pass (bottom = 0)
        sec                     ; 2 28
        sbc HeroY               ; 3 31
        cmp #hero_P0_HEIGHT     ; 2 33
        bcs .noHero             ; 2 35
        tay                     ; 2 37
        lda (GfxPtr),y          ; 5 42  the tables are bottom row first
        sta CurGfx0             ; 3 45
        lda (ColPtr),y          ; 5 50
        sta CurCol0             ; 3 53
        bcc .heroDone           ; 3 56  (carry still clear)
.noHero lda #0
        sta CurGfx0
.heroDone
        lda #0                  ; 2 58  the shot: enabled on its line pair
        cpx ShotPair            ; 3 61
        bne .noShot             ; 2 63
        lda #2                  ; 2 65
.noShot sta CurShot             ; 3 68
        sta WSYNC               ; 3 71
        dex                     ; 2  2
        txa                     ; 2  4  the playfield's row: (pairs left - 1) / 4
        lsr                     ; 2  6
        lsr                     ; 2  8
        tay                     ; 2 10
        lda room_PF0,y          ; 4 14
        sta PF0                 ; 3 17
        lda room_PF1,y          ; 4 21
        sta PF1                 ; 3 24
        lda room_PF2,y          ; 4 28
        sta PF2                 ; 3 31
        txa                     ; 2 33  the enemy's row on the next pass
        sec                     ; 2 35
        sbc EnemyRow            ; 3 38
        cmp #enemy_P1_HEIGHT    ; 2 40
        bcs .noEnemy            ; 2 42
        tay                     ; 2 44
        lda (EnemyPtr),y        ; 5 49
        bcc .enemyDone          ; 3 52
.noEnemy lda #0
.enemyDone
        sta CurGfx1             ; 3 55
        cpx #0                  ; 2 57
        bne .pair               ; 3 60

        sta WSYNC
        lda #0
        sta GRP0
        sta GRP1
        sta ENAM0
        sta PF0
        sta PF1
        sta PF2
        rts

; ---- PrepareScore: the digits' rows for the kernel. Each font byte has the
; digit in both halves; the score takes the tens' left half and the units'
; right one, and the lives' digit goes on the right half of the screen.
PrepareScore SUBROUTINE
        lda Score
        and #$0F
        jsr TimesFive
        sta Temp                ; the units' first row
        lda Score
        lsr
        lsr
        lsr
        lsr
        jsr TimesFive
        sta Temp+1              ; the tens'
        lda Lives
        jsr TimesFive
        sta Temp2               ; the lives'
        ldy #0
.row    ldx Temp2
        lda Digits,x
        and #$0F                ; the lives: the right half of PF1
        sta LivesRows,y
        ldx Temp+1
        lda Digits,x
        and #$F0                ; the tens: the left half
        sta ScoreRows,y
        ldx Temp
        lda Digits,x
        and #$0F                ; the units: the right half
        ora ScoreRows,y
        sta ScoreRows,y
        inc Temp
        inc Temp+1
        inc Temp2
        iny
        cpy #5
        bne .row
        rts

TimesFive                       ; A = A * 5 (A up to 51)
        sta Temp2
        asl
        asl
        clc
        adc Temp2
        rts

; THE GAME, 48 pixels wide: six columns of 8, two lines per row, bottom first.
TheGame0
        .byte %00100010  ; ..X...X.
        .byte %00100010  ; ..X...X.
        .byte %00100010  ; ..X...X.
        .byte %00100010  ; ..X...X.
        .byte %00100010  ; ..X...X.
        .byte %00100010  ; ..X...X.
        .byte %00100011  ; ..X...XX
        .byte %00100011  ; ..X...XX
        .byte %00100010  ; ..X...X.
        .byte %00100010  ; ..X...X.
        .byte %00100010  ; ..X...X.
        .byte %00100010  ; ..X...X.
        .byte %11111010  ; XXXXX.X.
        .byte %11111010  ; XXXXX.X.
TheGame1
        .byte %00101111  ; ..X.XXXX
        .byte %00101111  ; ..X.XXXX
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %11101111  ; XXX.XXXX
        .byte %11101111  ; XXX.XXXX
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %00101111  ; ..X.XXXX
        .byte %00101111  ; ..X.XXXX
TheGame2
        .byte %10000000  ; X.......
        .byte %10000000  ; X.......
        .byte %00000000  ; ........
        .byte %00000000  ; ........
        .byte %00000000  ; ........
        .byte %00000000  ; ........
        .byte %00000000  ; ........
        .byte %00000000  ; ........
        .byte %00000000  ; ........
        .byte %00000000  ; ........
        .byte %00000000  ; ........
        .byte %00000000  ; ........
        .byte %10000000  ; X.......
        .byte %10000000  ; X.......
TheGame3
        .byte %01111010  ; .XXXX.X.
        .byte %01111010  ; .XXXX.X.
        .byte %10001010  ; X...X.X.
        .byte %10001010  ; X...X.X.
        .byte %10001010  ; X...X.X.
        .byte %10001010  ; X...X.X.
        .byte %10011011  ; X..XX.XX
        .byte %10011011  ; X..XX.XX
        .byte %10000010  ; X.....X.
        .byte %10000010  ; X.....X.
        .byte %10000010  ; X.....X.
        .byte %10000010  ; X.....X.
        .byte %01111001  ; .XXXX..X
        .byte %01111001  ; .XXXX..X
TheGame4
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %00101000  ; ..X.X...
        .byte %11101010  ; XXX.X.X.
        .byte %11101010  ; XXX.X.X.
        .byte %00101010  ; ..X.X.X.
        .byte %00101010  ; ..X.X.X.
        .byte %00101101  ; ..X.XX.X
        .byte %00101101  ; ..X.XX.X
        .byte %11001000  ; XX..X...
        .byte %11001000  ; XX..X...
TheGame5
        .byte %10111110  ; X.XXXXX.
        .byte %10111110  ; X.XXXXX.
        .byte %10100000  ; X.X.....
        .byte %10100000  ; X.X.....
        .byte %10100000  ; X.X.....
        .byte %10100000  ; X.X.....
        .byte %10111100  ; X.XXXX..
        .byte %10111100  ; X.XXXX..
        .byte %10100000  ; X.X.....
        .byte %10100000  ; X.X.....
        .byte %10100000  ; X.X.....
        .byte %10100000  ; X.X.....
        .byte %10111110  ; X.XXXXX.
        .byte %10111110  ; X.XXXXX.

; The digits 0-9, five rows each, top first, in both halves of the byte.
Digits  .byte $EE,$AA,$AA,$AA,$EE  ; 0
        .byte $22,$22,$22,$22,$22  ; 1
        .byte $EE,$22,$EE,$88,$EE  ; 2
        .byte $EE,$22,$66,$22,$EE  ; 3
        .byte $AA,$AA,$EE,$22,$22  ; 4
        .byte $EE,$88,$EE,$22,$EE  ; 5
        .byte $EE,$88,$EE,$AA,$EE  ; 6
        .byte $EE,$22,$22,$22,$22  ; 7
        .byte $EE,$AA,$EE,$AA,$EE  ; 8
        .byte $EE,$AA,$EE,$22,$EE  ; 9

; ============================================================================
; Shared routines
; ============================================================================

; ---- Animate: advance the hero's animation, point at its frame and copy the
; frame's colours for the TV system. Run in the overscan, for the next frame.
Animate SUBROUTINE
        ldx Anim
        lda AnimTicksLo,x
        sta Temp
        lda AnimTicksHi,x
        sta Temp+1
        ldy Step
        inc Timer
        lda Timer
        cmp (Temp),y            ; done with this step?
        bcc .show
        lda #0
        sta Timer
        iny
        tya
        cmp AnimSteps,x
        bcc .keep
        ldy #0                  ; past the last step: back to the first...
        lda AnimLoop,x
        bne .keep
        ldy Step                ; ...unless it does not loop
.keep   sty Step
.show   lda AnimFrameLo,x
        sta Temp
        lda AnimFrameHi,x
        sta Temp+1
        ldy Step
        lda (Temp),y            ; the frame of this step
        tay
        lda hero_P0_FramePtrLo,y
        sta GfxPtr
        lda hero_P0_FramePtrHi,y
        sta GfxPtr+1
        lda hero_P0_ColourPtrLo,y
        sta Temp
        lda hero_P0_ColourPtrHi,y
        sta Temp+1
        ldy #hero_P0_HEIGHT-1
.colour lda (Temp),y
        jsr ConvertColour
        sta HeroColours,y
        dey
        bpl .colour
        lda #<HeroColours
        sta ColPtr
        lda #>HeroColours
        sta ColPtr+1
        rts

; The animations by number (Anim): their tables come from hero.gfx.asm.
AnimFrameLo     .byte <hero_idle_FRAME, <hero_walk_FRAME
AnimFrameHi     .byte >hero_idle_FRAME, >hero_walk_FRAME
AnimTicksLo     .byte <hero_idle_TICKS, <hero_walk_TICKS
AnimTicksHi     .byte >hero_idle_TICKS, >hero_walk_TICKS
AnimSteps       .byte hero_idle_STEPS, hero_walk_STEPS
AnimLoop        .byte hero_idle_LOOP, hero_walk_LOOP

; ---- ConvertColour: A = an NTSC colour; returns the colour for the TV system
; (PAL and PAL60 change the hue, the high nibble). Keeps X and Y.
ConvertColour SUBROUTINE
        sta ColourKeep
        lda System
        bne .pal
        lda ColourKeep          ; NTSC: as it is
        rts
.pal    lda ColourKeep
        and #$0F                ; the luminance stays
        sta ColourLum
        lda ColourKeep
        lsr
        lsr
        lsr
        lsr
        stx ColourKeep
        tax
        lda PalHues,x
        ora ColourLum
        ldx ColourKeep
        rts
; The PAL hue for each NTSC hue (0-F): the same table as the ELF loaders'
; ColorLookup, as measured for La Abadia del Crimen 2600.
PalHues .byte $00,$20,$20,$40,$60,$80,$A0,$C0,$D0,$B0,$90,$70,$50,$30,$20,$20

; ---- StickPresses: A = the joystick directions pressed on this frame and not
; on the last one, as 1 bits (D7 right, D6 left, D5 down, D4 up).
StickPresses SUBROUTINE
        lda SWCHA
        tax
        eor #$FF                ; 1: held now
        and LastStick           ; and not held before (1: was up)
        stx LastStick
        and #$F0
        rts

; ---- ButtonPress: carry set when the fire button has just gone down.
ButtonPress SUBROUTINE
        lda INPT4
        and #$80                ; D7 = 0: the button is down
        cmp Button              ; carry clear: down now, up before
        sta Button
        bcs .no
        sec
        rts
.no     clc
        rts

; ---- Wait12: jsr and rts take 12 cycles: a short wait in a kernel.
Wait12  rts

; ---- BlankLines: X = lines with nothing drawn.
BlankLines SUBROUTINE
.line   sta WSYNC
        dex
        bne .line
        rts

; ---- PosObject: A = x (0-159), X = object (0 P0, 1 P1, 2 M0). Ends with
; HMxx set: an HMOVE afterwards puts it in place.
PosObject SUBROUTINE
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

        include "hero.gfx.asm"
        include "enemy.gfx.asm"
        include "room.gfx.asm"
        include "title.gfx.asm"
        include "theme.music.asm"

        ORG $FFFC
        .word Reset
        .word Reset
