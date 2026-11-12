; {{NAME}} - made with VCSIDE
; Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
; VCSIDE The Game on a DPC+ cartridge (32K), {{TV}} at start.
; Build: F7 (or Build > Build). Run: F5.
;
; A room with walls where the hero walks and shoots at two enemies. Hitting
; one scores a point; touching one costs a life. A title screen with a menu
; (PLAY, and the TV system) comes first; with no lives left, or with RESET,
; the game goes back to it.
;
; What DPC+ adds to the 4K game:
; - Music played by the cartridge: its ARM plays three voices with 32-sample
;   waveforms and adds them up; the 6507 copies that sum (AMPLITUDE) to AUDV0
;   on every line, with AUDC0 = 0 (SOUND and FAST_SOUND below).
; - Data fetchers: the kernels read the graphics from the display data (4K of
;   the cartridge's RAM) with "lda #<DFxDATA" (fast fetch: 2 cycles), so a
;   line has time for the sound and for the playfield, the sprites and the
;   shot. A windowed fetcher (DFxDATAW) gives 0 outside its window: the hero
;   and the enemies need no test in the kernel. A fractional one
;   (DFxFRACDATA) reads each byte several times: one playfield row a byte.
; - A random number generator (RANDOM0NEXT).
;
; Graphics (open them from the project panel; saving one writes its .gfx.asm):
;   hero.vcsgfx   the hero (P0) and its animations, idle and walk
;   enemy.vcsgfx  the enemy (P1), one colour (its first row's)
;   room.vcsgfx   the room's playfield: 22 rows of 8 lines, mirrored
;   title.vcsgfx  the title's cartridge (VCSIDE's icon) and the menu's texts
; THE GAME is written below, in the display data (TheGame0D-5D).
; Music and sound: theme.vcsmusic (saving it writes theme.music.asm) is the
; song, played on both screens, and three effects: shot (takes the third
; voice), hit (on the TIA's channel 1) and lose (the third voice).
;
; DPCplus.arm is DPC+'s ARM driver, copied from batari Basic when the project
; was made (batari Basic: https://github.com/batari-Basic/batari-Basic).
;
; The cartridge, as the 6507 sees it: the driver (3K), six 4K banks (the game
; is in the last one, bank 5, where DPC+ starts), the display data (4K) and
; the frequency table (1K). The first 128 bytes of every bank are the
; cartridge's registers, so the code starts after them.
;
; How it works:
; - The frame: VSYNC, the vertical blank (the game logic, timed by the RIOT),
;   192 visible lines, and the overscan (the collisions, the hero's next
;   frame). PAL has 25 more blank lines above and below the same 192 (312 in
;   all); NTSC and PAL60 have 262. The colours are written for NTSC and turned
;   into PAL ones (ConvertColour).
; - At power on, Reset copies the playfields of room.vcsgfx and title.vcsgfx
;   to the display data, top row first, for the fetchers (CopyTables).
; - The game's kernel draws two lines per pass: the first writes the
;   playfield, the second the sprites and the shot (the ball).
; - The two enemies take turns on player 1, one per frame (flicker).
; - The TIA says what touched what (collision registers): the hero and an
;   enemy go back where they were when they touch a wall, the shot ends there.

	processor 6502
	include "2600.h"

; ---- DPC+ registers (the ones this program uses) -----------------------------
; Read. With fast fetch on, "lda #<register" reads them too (only lda #n with
; n < $28): FAST_SOUND and the kernels do.
RANDOM0NEXT    = $1000          ; the next random number (one byte of its 32 bits)
AMPLITUDE      = $1005          ; the sum of the three voices, for AUDV0
DF0DATA        = $1008          ; DFxDATA: the next byte of fetcher x
DF1DATA        = $1009
DF2DATA        = $100A
DF3DATA        = $100B
DF4DATA        = $100C
DF5DATA        = $100D
DF0DATAW       = $1010          ; DFxDATAW: the same, or 0 outside its window
DF2DATAW       = $1012
DF3DATAW       = $1013
DF0FRACDATA    = $1018          ; DFxFRACDATA: the fractional fetcher x
DF1FRACDATA    = $1019
DF2FRACDATA    = $101A
DF3FRACDATA    = $101B
DF4FRACDATA    = $101C
DF5FRACDATA    = $101D
DF6FRACDATA    = $101E
DF7FRACDATA    = $101F
; Write.
DF0FRACLOW     = $1028          ; a fractional fetcher's pointer, low byte...
DF0FRACHI      = $1030          ; ...and high byte (writing them resets its fraction)
DF0FRACINC     = $1038          ; what each read adds, in 256ths of a byte
DF0TOP         = $1040          ; the window: from the low byte TOP+1...
DF1TOP         = $1041
DF2TOP         = $1042
DF3TOP         = $1043
DF0BOT         = $1048          ; ...to BOT-1
DF2BOT         = $104A
DF3BOT         = $104B
DF0LOW         = $1050          ; a fetcher's pointer, low byte...
DF0HI          = $1068          ; ...and high byte (DFxLOW = DF0LOW+x, DFxHI = DF0HI+x)
DF7LOW         = $1057
DF7HI          = $106F
FASTFETCH      = $1058          ; 0: fast fetch on; any other value: off
WAVEFORM0      = $105D          ; a voice's waveform (the music player writes them)
WAVEFORM1      = $105E
WAVEFORM2      = $105F
NOTE0          = $1075          ; a voice's entry in the frequency table
NOTE1          = $1076
NOTE2          = $1077
DF7WRITE       = $107F          ; a byte to the display data at DF7's pointer, which moves on
BANK4          = $1FFA          ; write (or read): selects bank 4...
BANK5          = $1FFB          ; ...or bank 5

; ---- Settings ----------------------------------------------------------------
DEFAULT_SYSTEM = {{TV_SYSTEM}}  ; the TV system at start (the title's menu changes it): 0 NTSC, 1 PAL60, 2 PAL
LIVES          = 3
ROOM_PAIRS     = room_ROWS * room_LINES / 2  ; 88 line pairs of room
HERO_START_X   = 76
HERO_START_Y   = 8              ; line pair of the hero's bottom row (1 = the bottom one)
SHOT_SPEED     = 3
TEXT_ROWS      = play_ROWS      ; the menu's texts (PLAY, NTSC, PAL60, PAL): rows and lines
TEXT_LINES     = play_ROWS * play_LINES
CART_INC       = [256 + cart_LINES - 1] / cart_LINES  ; the fractional fetchers' step: a row
TEXT_INC       = [256 + play_LINES - 1] / play_LINES  ; every _LINES lines
	IF cart_LINES < 2 || play_LINES < 2
	ERR ; title.vcsgfx: the fractional fetchers need rows of 2 lines or more
	ENDIF
	IF ntsc_ROWS != TEXT_ROWS || pal60_ROWS != TEXT_ROWS || pal_ROWS != TEXT_ROWS
	ERR ; title.vcsgfx: PLAY, NTSC, PAL60 and PAL must have the same rows
	ENDIF

; ---- SOUND: the voices' sum to AUDV0. Every line does it at least once; code
; that takes longer than a line does it in between, or the sound stops.
; FAST_SOUND is the same in 5 cycles, for the kernels (fast fetch on).
	MAC SOUND
	lda AMPLITUDE
	sta AUDV0
	ENDM
	MAC FAST_SOUND
	lda #<AMPLITUDE
	sta AUDV0
	ENDM

; ---- RAM -------------------------------------------------------------------
	SEG.U Variables
	ORG $80
System    ds 1          ; 0 NTSC, 1 PAL60, 2 PAL
Mode      ds 1          ; 0 the title screen, 1 the game
DrawMode  ds 1          ; Mode when the frame began: what this frame draws
MenuRow   ds 1          ; 0 PLAY, 1 TV
LastStick ds 1          ; SWCHA on the last frame (0 bits: directions held)
Button    ds 1          ; the fire button on the last frame (bit 7: 0 down)
Frame     ds 1
CartSystem ds 1         ; the system CartColD was converted for ($FF: none yet)
HeroX     ds 1          ; colour clocks from the left
HeroY     ds 1          ; line pair of the hero's bottom row, from the bottom
OldHeroX  ds 1          ; where it was before this frame's move
OldHeroY  ds 1
Facing    ds 1          ; REFP0: 0 right, 8 left
Anim      ds 1          ; 0 idle, 1 walk (the order of the tables below)
Step      ds 1          ; step of the animation
Timer     ds 1          ; frames into the step
GfxPtr    ds 2          ; the hero's frame (bottom row first)
ColPtr    ds 2          ; its colours
Temp      ds 2          ; scratch: pointers and the score's digits
Temp2     ds 1
TextColour  ds 1        ; THE GAME's colour and the lives', worked out in the vertical blank
ColourKeep  ds 1        ; ConvertColour's own
SidesColour ds 1        ; the cartridge label's sides, for the TV system
MusicRam  ds 29         ; the music player's: 5 bytes, 7 per voice, 3 for the TIA's effects
MusicRamEnd
MUSIC_RAM = MusicRam

; The game's state: StartGame clears it.
Shared
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
ScoreRows ds 5          ; the score's two digits, PF1, top row first
LivesRows ds 5          ; the lives' digit
EnemyColour ds 1        ; COLUP1 for the shown enemy, for the TV system
GameEnd
	IF GameEnd > $F6
		ERR ; not enough RAM: the stack needs the last 10 bytes
	ENDIF

; ---- The ARM driver --------------------------------------------------------
	SEG Code
	ORG $0400
	RORG $0000
	incbin "DPCplus.arm"

; ---- Calling bank 4 from bank 5: a trampoline of 10 bytes at the same address
; in both banks. Bank 5 writes BANK4 and its next instruction comes from bank 4,
; which calls the routine (A, X and Y as the caller left them) and writes BANK5:
; the next instruction is bank 5's rts. A cartridge starts in bank 5, and every
; bank's reset vector goes there.
TRAMPOLINES    = $FFC0          ; three trampolines, before the reset code at $FFF0
	MAC CALL_BANK4          ; in bank 5: {1} the name to jsr to
{1}     sta BANK4               ; +0
	ds 6, $EA               ; +3: bank 4's jsr and sta BANK5 (not run in bank 5)
	rts                     ; +9
	ENDM
	MAC IN_BANK4            ; in bank 4: {1} the routine
	ds 3, $EA               ; +0: bank 5's sta BANK4
	jsr {1}                 ; +3
	sta BANK5               ; +6
	ds 1, $EA               ; +9: bank 5's rts
	ENDM

; ---- Banks 0-3: unused; a reset there goes to bank 5 -------------------------
	MAC UNUSED_BANK         ; {1}: the bank's place in the file
	ORG {1}+$0FF0
	RORG $FFF0
	sta BANK5               ; the next instruction comes from bank 5, at the same address
	jmp Reset
	ORG {1}+$0FFC
	RORG $FFFC
	.word $FFF0, $FFF0
	ENDM
	UNUSED_BANK $1000
	UNUSED_BANK $2000
	UNUSED_BANK $3000
	UNUSED_BANK $4000

; ---- Bank 4: the music player and the song ---------------------------------
; Bank 5 calls them through trampolines (CALL_BANK4 below), at the same
; addresses in both banks.
	ORG $5000
	RORG $F000
	ds.b 128, 0             ; under the cartridge's registers
MUSIC_PART SET 1
	include "theme.music.asm"
	IF MusicRam + MUSIC_RAM_BYTES > MusicRamEnd
	ERR                     ; MusicRam is too small: make it MUSIC_RAM_BYTES long
	ENDIF
	ORG $5000+TRAMPOLINES-$F000
	RORG TRAMPOLINES
	IN_BANK4 MusicInit
	IN_BANK4 MusicFrame
	IN_BANK4 MusicEffect
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
	lda #$FF
	sta FASTFETCH           ; fast fetch off: lda # loads numbers
	sta CartSystem          ; the cartridge's colours: not converted yet
	sta LastStick
	jsr CopyTables
	lda #DEFAULT_SYSTEM
	sta System
	lda #$80
	sta Button              ; as if up: holding it at power on does nothing
	jsr PlayerInit          ; the song from its start (AUDC0 is 0 already)

StartFrame SUBROUTINE
	lda #2
	sta WSYNC               ; VSYNC from the start of a line
	sta VBLANK
	sta VSYNC
	ldx #2
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
	inc Frame
	lda Mode
	sta DrawMode            ; what this frame draws, whatever the logic decides
	jsr PlayerFrame         ; the voices' waveforms and notes for this frame
	SOUND
	lda Mode
	bne .game
	jsr TitleLogic
	jmp .wait
.game
	jsr GameLogic
.wait
	sta WSYNC               ; until the timer runs out, a line at a time
	SOUND
	bit TIMINT              ; D7: the timer has run out (it stays set)
	bpl .wait
	lda DrawMode            ; each kernel turns the beam on (BeamOn) after
	bne .drawGame           ; setting its registers
	bne .drawGame
	jsr TitleKernel
	jmp .overscan
.drawGame
	jsr GameKernel

.overscan
	lda #2
	sta VBLANK
	sta WSYNC               ; the timer from the start of a line: always the same length
	SOUND
	ldx System
	lda OverscanTime,x
	sta TIM64T
	lda DrawMode
	beq .title
	jsr Collisions
	SOUND
	jsr PrepareScore        ; the score and the lives for the next frame
	jmp .animate
.title
	lda System              ; the title: its colours, when the system has changed
	cmp CartSystem
	beq .animate
	jsr CartColoursForTv
	jmp .over
.animate
	jsr Animate             ; the hero for the next frame (on the title too, so
.over
	sta WSYNC               ; it is ready when the game starts)
	SOUND
	bit TIMINT
	bpl .over
	jmp StartFrame

; Timer values (TIM64T, 64 cycles each) for the blank lines of each TV system:
; 262 lines (NTSC, PAL60) and 312 (PAL: 25 more above and below). A timer that
; runs out before the program waits for it makes the frame longer: keep the
; vertical blank's work inside its time.
VblankTime      .byte 42, 42, 72
OverscanTime    .byte 33, 33, 62

; ---- CopyTables: the playfields of title.vcsgfx and room.vcsgfx to the
; display data, top row first (their tables are bottom row first), in the
; order of CopyList, which is the order of the display data from CartPF0D.
CopyTables SUBROUTINE
	lda #<CartPF0D
	sta DF7LOW
	lda #>CartPF0D
	sta DF7HI
	ldx #0
.table
	lda CopyList,x
	sta Temp
	lda CopyList+1,x
	sta Temp+1
	ldy CopyList+2,x
	dey
.row
	lda (Temp),y
	sta DF7WRITE
	dey
	bpl .row
	inx
	inx
	inx
	cpx #CopyListEnd-CopyList
	bne .table
	rts

	MAC COPY                ; {1}: a table, {2}: its rows
	.byte <{1}, >{1}, {2}
	ENDM
CopyList
	COPY cart_PF0, cart_ROWS
	COPY cart_PF1, cart_ROWS
	COPY cart_PF2, cart_ROWS
	COPY cart_PF0R, cart_ROWS
	COPY cart_PF1R, cart_ROWS
	COPY cart_PF2R, cart_ROWS
	COPY play_PF0, TEXT_ROWS
	COPY ntsc_PF0, TEXT_ROWS
	COPY pal60_PF0, TEXT_ROWS
	COPY pal_PF0, TEXT_ROWS
	COPY play_PF1, TEXT_ROWS
	COPY ntsc_PF1, TEXT_ROWS
	COPY pal60_PF1, TEXT_ROWS
	COPY pal_PF1, TEXT_ROWS
	COPY play_PF2, TEXT_ROWS
	COPY ntsc_PF2, TEXT_ROWS
	COPY pal60_PF2, TEXT_ROWS
	COPY pal_PF2, TEXT_ROWS
	COPY play_PF0R, TEXT_ROWS
	COPY ntsc_PF0R, TEXT_ROWS
	COPY pal60_PF0R, TEXT_ROWS
	COPY pal_PF0R, TEXT_ROWS
	COPY play_PF1R, TEXT_ROWS
	COPY ntsc_PF1R, TEXT_ROWS
	COPY pal60_PF1R, TEXT_ROWS
	COPY pal_PF1R, TEXT_ROWS
	COPY play_PF2R, TEXT_ROWS
	COPY ntsc_PF2R, TEXT_ROWS
	COPY pal60_PF2R, TEXT_ROWS
	COPY pal_PF2R, TEXT_ROWS
	COPY room_PF0, room_ROWS
	COPY room_PF1, room_ROWS
	COPY room_PF2, room_ROWS
CopyListEnd

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
.noRow
	lda MenuRow
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
.play
	jsr ButtonPress
	bcc .place
	jsr StartGame           ; the game from the next frame (DrawMode)
.place  SOUND
	lda #$9C                ; THE GAME: light blue, for the TV system
	jsr ConvertColour
	sta TextColour
	lda #CART_SIDES
	jsr ConvertColour
	sta SidesColour
	; THE GAME: players 0 and 1, three close copies each, 8 pixels apart
	lda #$03
	sta NUSIZ0
	sta NUSIZ1
	lda #1
	sta VDELP0
	sta VDELP1
	lda #0
	sta REFP0
	lda #TEXT48_X
	ldx #0
	jsr PosObject
	lda #TEXT48_X+8
	ldx #1
	jsr PosObject
	; the cartridge label's sides: the missiles, on its edge columns (9 and 29)
	lda #CART_LEFT
	ldx #2
	jsr PosObject
	lda #CART_RIGHT
	ldx #3
	jsr PosObject
	sta WSYNC
	sta HMOVE
	SOUND
	; the fetchers, one a line: the cartridge's eight fractional ones, and
	; THE GAME's six columns
	ldx #0
.fetcher
	sta WSYNC
	SOUND
	lda CartTablesLo,x
	sta DF0FRACLOW,x
	lda CartTablesHi,x
	sta DF0FRACHI,x
	lda #CART_INC
	sta DF0FRACINC,x
	cpx #6
	bcs .next
	lda TheGameLo,x
	sta DF0LOW,x
	lda TheGameHi,x
	sta DF0HI,x
.next
	inx
	cpx #8
	bne .fetcher
	rts

; the cartridge's fetchers: DF0-5 its playfield, DF6 the row colours, DF7 the sides
CartTablesLo .byte <CartPF0D, <CartPF1D, <CartPF2D, <CartPF0RD, <CartPF1RD, <CartPF2RD, <CartColD, <CartSidesD
CartTablesHi .byte >CartPF0D, >CartPF1D, >CartPF2D, >CartPF0RD, >CartPF1RD, >CartPF2RD, >CartColD, >CartSidesD
TheGameLo    .byte <TheGame0D, <TheGame1D, <TheGame2D, <TheGame3D, <TheGame4D, <TheGame5D
TheGameHi    .byte >TheGame0D, >TheGame1D, >TheGame2D, >TheGame3D, >TheGame4D, >TheGame5D

NextSystem SUBROUTINE
	inc System
	lda System
	cmp #3
	bcc .ok
	lda #0
	sta System
.ok
	rts

; ---- TitleKernel: 192 lines: VCSIDE's cartridge (playfield and missiles),
; THE GAME (48 pixels of players), PLAY and the TV system. The selected row is
; white, the other grey. Fast fetch is on: constants go in X and Y.
TitleKernel SUBROUTINE
	lda #0
	sta ENAM0
	sta ENAM1
	sta CTRLPF              ; the texts are not mirrored
	sta COLUBK
	sta GRP0                ; no players (with VDELP0/1 on, each one twice)
	sta GRP1
	sta GRP0
	lda SidesColour         ; the missiles: the label's grey sides, 4 clocks wide
	sta COLUP0
	sta COLUP1
	lda #$20
	sta NUSIZ0
	sta NUSIZ1
	jsr BeamOn
	ldx #10
	jsr BlankLines
	jsr Cartridge
	ldx #$03                ; the players for THE GAME: three close copies each
	stx NUSIZ0
	stx NUSIZ1
	lda TextColour
	sta COLUP0
	sta COLUP1
	ldx #8
	jsr BlankLines
	jsr Text48
	ldx #0                  ; PLAY's fetchers
	jsr PointText
	ldx #16-6
	jsr BlankLines
	ldy #$0E                ; white when selected
	lda MenuRow
	beq .playColour
	ldy #$06
.playColour
	sty COLUPF
	jsr DrawText
	ldx System              ; the TV system's text: NTSC, PAL60 or PAL
	inx
	jsr PointText
	ldx #8-6
	jsr BlankLines
	ldy #$0E
	lda MenuRow
	bne .tvColour
	ldy #$06
.tvColour
	sty COLUPF
	jsr DrawText
	ldx #192-10-[cart_ROWS*cart_LINES+1]-8-[TEXT48_LINES+1]-16-[TEXT_LINES+1]-8-[TEXT_LINES+1]
	jsr BlankLines
	ldx #$FF
	stx FASTFETCH           ; fast fetch off
	rts

; ---- Cartridge: the cartridge of VCSIDE's icon, a line at a time from eight
; fractional fetchers (one row every cart_LINES lines): the row's colour, its
; asymmetric playfield, and the label's grey sides, which are the two missiles
; (a playfield line has one colour), on from the second row (after the lip).
CART_SIDES  = $06               ; the body's grey in title.vcsgfx
CART_LEFT   = 34                ; PosObject's x for the missiles over columns 9 and 29
CART_RIGHT  = 114               ; (pixels 36 and 116): it puts a missile 2 pixels right
Cartridge SUBROUTINE
	ldx #cart_ROWS*cart_LINES
.line
	sta WSYNC
	lda #<AMPLITUDE         ; 2
	sta AUDV0               ; 5
	lda #<DF6FRACDATA       ; 7  the row's colour
	sta COLUPF              ; 10
	lda #<DF0FRACDATA       ; 12
	sta PF0                 ; 15
	lda #<DF1FRACDATA       ; 17
	sta PF1                 ; 20
	lda #<DF7FRACDATA       ; 22  the sides: 0 or 2, before the left one (cycle 35)
	sta ENAM0               ; 25
	sta ENAM1               ; 28
	lda #<DF2FRACDATA       ; 30
	sta PF2                 ; 33
	lda #<DF3FRACDATA       ; 35
	sta PF0                 ; 38  the right PF0: from cycle 28 to 49
	lda #<DF4FRACDATA       ; 40
	sta PF1                 ; 43  the right PF1: from cycle 39 to 54
	lda #<DF5FRACDATA       ; 45
	nop                     ; 47
	sta PF2                 ; 50  the right PF2: from cycle 50 to 65
	dex                     ; 52
	bne .line               ; 55
	sta WSYNC
	FAST_SOUND
	ldx #0
	stx ENAM0
	stx ENAM1
	stx PF0
	stx PF1
	stx PF2
	rts

; ---- CartColoursForTv: the cartridge's row colours (cart_COLUPF_ROWS, bottom
; first) to CartColD, top first, converted for the TV system (the overscan).
CartColoursForTv SUBROUTINE
	lda System
	sta CartSystem
	lda #<CartColD
	sta DF7LOW
	lda #>CartColD
	sta DF7HI
	ldy #cart_ROWS-1
	lda System
	bne .pal
.ntsc
	lda cart_COLUPF_ROWS,y  ; NTSC: as they are
	sta DF7WRITE
	SOUND
	dey
	bpl .ntsc
	rts
.pal
	lda cart_COLUPF_ROWS,y
	lsr
	tax
	lda NtscToPal,x
	sta DF7WRITE
	SOUND
	dey
	bpl .pal
	rts

; ---- Text48: THE GAME, 48 pixels wide. Players 0 and 1 have three copies
; each, 16 pixels apart, and P1 starts 8 pixels after P0: six columns of 8,
; read from fetchers 0-5. With VDELP0 and VDELP1 each player shows the value
; its register had when the other one was last written, so the line writes
; the first three columns early and the last three just as the beam reaches
; them (cycles 44, 47, 50 and 53). The 48-pixel technique of Activision's
; six-digit scores. The text is from pixel 55 to 101, centred.
TEXT48_X     = 52               ; PosObject's x for P0: it lands on pixel 55
TEXT48_LINES = 14
Text48 SUBROUTINE
	ldy #TEXT48_LINES
	sty Temp2
.line
	sta WSYNC
	lda #<AMPLITUDE         ; 2
	sta AUDV0               ; 5
	lda #<DF0DATA           ; 7
	sta GRP0                ; 10  column 1 (waits in the delayed GRP0)
	lda #<DF1DATA           ; 12
	sta GRP1                ; 15  column 2; column 1 shows
	lda #<DF2DATA           ; 17
	sta GRP0                ; 20  column 3; column 2 shows
	lda #<DF3DATA           ; 22
	sta Temp                ; 25
	lda #<DF4DATA           ; 27
	tax                     ; 29
	lda #<DF5DATA           ; 31
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
	FAST_SOUND
	ldx #0
	stx GRP0
	stx GRP1
	stx GRP0
	rts

; ---- PointText: X = a text (0 PLAY, 1 NTSC, 2 PAL60, 3 PAL): fractional
; fetchers 0-5 at its rows, one fetcher a line (6 lines).
PointText SUBROUTINE
	ldy #>TextPF0D
	sty Temp+1
	lda TextOffsets,x
	clc
	adc #<TextPF0D
	sta Temp
	bcc .first
	inc Temp+1
.first
	ldx #0
.fetcher
	sta WSYNC
	lda #<AMPLITUDE
	sta AUDV0
	lda Temp
	sta DF0FRACLOW,x
	lda Temp+1
	sta DF0FRACHI,x
	ldy #TEXT_INC
	tya
	sta DF0FRACINC,x
	lda Temp                ; the next register's texts
	clc
	adc #TEXT_ROWS*4
	sta Temp
	bcc .same
	inc Temp+1
.same
	inx
	cpx #6
	bne .fetcher
	rts
TextOffsets .byte 0, TEXT_ROWS, TEXT_ROWS*2, TEXT_ROWS*3

; ---- DrawText: TEXT_LINES lines of a text of title.vcsgfx, from fractional
; fetchers 0-5 (PointText). Its rows are 40 columns (asymmetric): each line
; writes PF0-PF2 for the left half and again, in time, for the right one.
DrawText SUBROUTINE
	ldx #TEXT_LINES
.line
	sta WSYNC
	lda #<AMPLITUDE         ; 2
	sta AUDV0               ; 5
	lda #<DF0FRACDATA       ; 7
	sta PF0                 ; 10
	lda #<DF1FRACDATA       ; 12
	sta PF1                 ; 15
	lda #<DF2FRACDATA       ; 17
	sta PF2                 ; 20
	lda #<DF3FRACDATA       ; 22
	bit Temp                ; 25
	sta PF0                 ; 28  the right PF0: from cycle 28 to 49
	lda #<DF4FRACDATA       ; 30
	nop                     ; 32
	nop                     ; 34
	nop                     ; 36
	sta PF1                 ; 39  the right PF1: from cycle 39 to 54
	lda #<DF5FRACDATA       ; 41
	nop                     ; 43
	nop                     ; 45
	nop                     ; 47
	sta PF2                 ; 50  the right PF2: from cycle 50 to 65
	dex                     ; 52
	bne .line               ; 55
	sta WSYNC
	FAST_SOUND
	ldx #0
	stx PF0
	stx PF1
	stx PF2
	rts

; ============================================================================
; The game
; ============================================================================

StartGame SUBROUTINE
	lda #0                  ; the game's state from zero
	ldx #GameEnd-Shared-1
.clear
	sta Shared,x
	dex
	bpl .clear
	lda #$F0                ; no enemy until GameLogic places one
	sta EnemyRow
	lda #1
	sta Mode
	lda #LIVES
	sta Lives
	jsr PrepareScore
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
.enemy
	lda WaveX,x
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
	lda #0                  ; back to the title from the next frame
	sta Mode
.noReset
	jsr MoveHero
	SOUND
	jsr Shoot
	SOUND
	jsr MoveEnemy
	SOUND
	; the colours for the TV system
	lda #room_COLUPF
	jsr ConvertColour
	sta COLUPF              ; the walls and the shot (the ball)
	lda #room_COLUBK
	jsr ConvertColour
	sta COLUBK
	SOUND
	lda enemy_P1_Colour0+enemy_P1_HEIGHT-1  ; the enemy's top row's colour
	ldx Shown
	beq .first
	clc
	adc #$40                ; the second enemy: another hue
.first
	jsr ConvertColour
	sta EnemyColour         ; COLUP1, written after the score
	lda #$1C                ; the lives: yellow
	jsr ConvertColour
	sta TextColour
	SOUND
	; the shown enemy's frame to the display data, top row first
	lda #<EnemyBuf
	sta DF7LOW
	lda #>EnemyBuf
	sta DF7HI
	ldy #enemy_P1_HEIGHT-1
.enemyRow
	lda (EnemyPtr),y
	sta DF7WRITE
	dey
	bpl .enemyRow
	SOUND
	jsr PointGame
	; the objects' places: P0, P1 and the ball
	lda Facing
	sta REFP0
	lda #0                  ; one copy of each player
	sta NUSIZ0
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
	ldx #4
	jsr PosObject
	sta WSYNC
	sta HMOVE
	SOUND
	rts

; ---- PointGame: the game kernel's fetchers. On pass k from the top (the
; room's line pair ROOM_PAIRS - k) a fetcher reads its pointer + k; the
; window lets through its buffer only, so pointer = buffer - the passes
; before the object's top row.
;   DF0 (windowed) the hero, DF1 its colours, DF2 (windowed) the enemy,
;   DF3 (windowed) the shot, DF4-DF6 (fractional) the room's PF0-PF2.
PointGame SUBROUTINE
	lda #<[HeroGfxBuf+hero_P0_HEIGHT-1-ROOM_PAIRS]
	clc
	adc HeroY
	sta DF0LOW
	lda #>[HeroGfxBuf+hero_P0_HEIGHT-1-ROOM_PAIRS]
	adc #0
	sta DF0HI
	lda #<[HeroColBuf+hero_P0_HEIGHT-1-ROOM_PAIRS]
	clc
	adc HeroY
	sta DF0LOW+1
	lda #>[HeroColBuf+hero_P0_HEIGHT-1-ROOM_PAIRS]
	adc #0
	sta DF0HI+1
	lda #<[EnemyBuf+enemy_P1_HEIGHT-1-ROOM_PAIRS]
	clc
	adc EnemyRow
	sta DF0LOW+2
	lda #>[EnemyBuf+enemy_P1_HEIGHT-1-ROOM_PAIRS]
	adc #0
	sta DF0HI+2
	SOUND
	lda #<[ShotBuf-ROOM_PAIRS]
	clc
	adc ShotPair
	sta DF0LOW+3
	lda #>[ShotBuf-ROOM_PAIRS]
	adc #0
	sta DF0HI+3
	lda #<[HeroGfxBuf-1]    ; the windows
	sta DF0TOP
	lda #<[HeroGfxBuf+hero_P0_HEIGHT]
	sta DF0BOT
	lda #<[EnemyBuf-1]
	sta DF2TOP
	lda #<[EnemyBuf+enemy_P1_HEIGHT]
	sta DF2BOT
	lda #<[ShotBuf-1]
	sta DF3TOP
	lda #<[ShotBuf+1]
	sta DF3BOT
	SOUND
	ldx #2                  ; the room: a byte every 4 line pairs
.room
	lda RoomTablesLo,x
	sta DF0FRACLOW+4,x
	lda RoomTablesHi,x
	sta DF0FRACHI+4,x
	lda #256/[room_LINES/2]
	sta DF0FRACINC+4,x
	dex
	bpl .room
	SOUND
	rts
RoomTablesLo .byte <RoomPF0D, <RoomPF1D, <RoomPF2D
RoomTablesHi .byte >RoomPF0D, >RoomPF1D, >RoomPF2D

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
.noLeft SOUND
	lda SWCHA
	and #$10                ; D4 = 0: up
	bne .noUp
	ldy #1
	lda HeroY
	cmp #ROOM_PAIRS-hero_P0_HEIGHT
	bcs .noUp
	inc HeroY
.noUp
	lda SWCHA
	and #$20                ; D5 = 0: down
	bne .noDown
	ldy #1
	lda HeroY
	cmp #2
	bcc .noDown
	dec HeroY
.noDown
	cpy Anim
	beq .same
	sty Anim                ; another animation: from its first step
	lda #0
	sta Step
	sta Timer
.same
	rts

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
.from
	sta ShotX
	stx ShotDX
	SOUND
	lda #MUSIC_EFFECT_SHOT
	jsr PlayerEffect
.move
	lda ShotPair
	beq .done
	lda ShotX
	clc
	adc ShotDX
	sta ShotX
	cmp #158                ; off the screen (on the left it wraps above 158)
	bcc .done
	lda #0
	sta ShotPair
.done
	rts

; ---- MoveEnemy: each enemy moves diagonally on every other frame (they take
; turns, 30 Hz each); the collisions turn it round. Both alive: they share
; player 1 and take turns on the screen too (30 Hz flicker). One alive: it is
; drawn on every frame, without flicker.
MoveEnemy SUBROUTINE
	lda #$F0                ; not drawn
	sta EnemyRow
	lda Frame
	and #1
	sta Temp                ; whose turn it is to move
	tax
	lda EnemyLive
	beq .shown              ; none: nothing drawn
	cmp #3
	beq .shown              ; both: the one whose turn it is
	lsr                     ; one: that one (1: enemy 0, 2: enemy 1)
	tax
.shown
	stx Shown
	lda EnemyLive
	and EnemyBit,x
	beq .anim
	lda EnemyX,x            ; where it was, for the collisions
	sta OldEnemyX
	lda EnemyY,x
	sta OldEnemyY
	cpx Temp
	bne .draw               ; not its turn: drawn where it is
	lda EnemyX,x
	clc
	adc EnemyDX,x
	sta EnemyX,x
	lda EnemyY,x
	clc
	adc EnemyDY,x
	sta EnemyY,x
.draw
	lda EnemyY,x
	sta EnemyRow
.anim   SOUND
	lda Frame
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
	lda EnemyRow
	cmp #$F0
	beq .enemyFree          ; no enemy was drawn
	bit CXP1FB              ; D7: the enemy and the playfield
	bpl .enemyFree
	lda OldEnemyX           ; back where it was...
	sta EnemyX,x
	lda OldEnemyY
	sta EnemyY,x
	lda RANDOM0NEXT         ; ...and the other way on one axis. The TIA does not
	and #1                  ; say which side it touched, so the axis is chosen at
	beq .turnY              ; random; a wrong guess touches again and turns the
	lda #0                  ; other one (a fixed rule traps them in a loop).
	sec
	sbc EnemyDX,x
	sta EnemyDX,x
	jmp .enemyFree
.turnY
	lda #0
	sec
	sbc EnemyDY,x
	sta EnemyDY,x
.enemyFree
	SOUND
	bit CXBLPF              ; D7: the shot (the ball) and the playfield
	bpl .shotFree
	lda #0
	sta ShotPair
.shotFree
	bit CXP1FB              ; D6: the shot and the enemy
	bvc .noHit
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
	SOUND
	lda #MUSIC_EFFECT_HIT
	jsr PlayerEffect
	lda EnemyLive
	bne .noHit
	jsr NewWave             ; both gone: two more
.noHit  SOUND
	bit CXPPMM              ; D7: the hero and the enemy
	bpl .done
	lda #MUSIC_EFFECT_LOSE
	jsr PlayerEffect
	jsr PlaceHero
	jsr NewWave             ; the enemies back at the top, away from the hero
	dec Lives
	bne .done
	lda #0                  ; no lives left: the title
	sta Mode
.done
	sta CXCLR
	rts

; ---- GameKernel: 192 lines: the score (16) and the room (176). Fast fetch is
; on: constants go in X and Y.
GameKernel SUBROUTINE
	; the score: the left half in COLUP0, the right one in COLUP1 (score mode)
	lda #$02
	sta CTRLPF
	lda #$0E
	sta COLUP0
	lda TextColour          ; the lives' colour
	sta COLUP1
	lda #0
	sta PF0
	sta PF1
	sta PF2
	sta GRP0
	sta GRP1
	sta ENABL
	jsr BeamOn
	ldx #3
	jsr BlankLines
	ldy #0
.digit
	ldx #2                  ; two lines per row
.digitLine
	sta WSYNC
	lda #<AMPLITUDE         ; 2
	sta AUDV0               ; 5
	lda ScoreRows,y         ; 9
	sta PF1                 ; 12  the left PF1: the score
	jsr Wait12              ;   24
	jsr Wait12              ;   36
	nop                     ;   38
	lda LivesRows,y         ; 4 42
	sta PF1                 ; 3 45  the right PF1 (cycles 39 to 54): the lives
	dex
	bne .digitLine
	iny
	cpy #5
	bne .digit
	sta WSYNC
	FAST_SOUND
	ldx #0
	stx PF1
	ldx #room_CTRLPF | $20  ; the room: mirrored, and a ball 4 clocks wide
	stx CTRLPF
	lda EnemyColour
	sta COLUP1
	sta WSYNC
	FAST_SOUND

; Two lines per pass, X = line pairs left (ROOM_PAIRS down to 1): the first
; writes the playfield, the second the sprites and the shot.
	ldx #ROOM_PAIRS
.pair
	sta WSYNC
	lda #<AMPLITUDE         ; 2
	sta AUDV0               ; 5
	lda #<DF4FRACDATA       ; 7
	sta PF0                 ; 10
	lda #<DF5FRACDATA       ; 12
	sta PF1                 ; 15
	lda #<DF6FRACDATA       ; 17
	sta PF2                 ; 20
	sta WSYNC
	lda #<AMPLITUDE         ; 2
	sta AUDV0               ; 5
	lda #<DF0DATAW          ; 7  the hero: 0 outside its window
	sta GRP0                ; 10
	lda #<DF1DATA           ; 12
	sta COLUP0              ; 15
	lda #<DF2DATAW          ; 17  the enemy
	sta GRP1                ; 20
	lda #<DF3DATAW          ; 22  the shot: 2 on its line pair
	sta ENABL               ; 25
	dex                     ; 27
	bne .pair               ; 30

	sta WSYNC
	FAST_SOUND
	ldx #0
	stx GRP0
	stx GRP1
	stx ENABL
	stx PF0
	stx PF1
	stx PF2
	dex
	stx FASTFETCH           ; $FF: fast fetch off
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
.row
	ldx Temp2
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
	SOUND
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

; ---- Animate: advance the hero's animation, and write its frame and the
; frame's colours (for the TV system) to the display data, top row first.
; Run in the overscan, for the next frame.
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
.keep
	sty Step
.show
	lda AnimFrameLo,x
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
	sta ColPtr
	lda hero_P0_ColourPtrHi,y
	sta ColPtr+1
	SOUND
	lda #<HeroGfxBuf        ; HeroColBuf follows it
	sta DF7LOW
	lda #>HeroGfxBuf
	sta DF7HI
	ldy #hero_P0_HEIGHT-1
.gfx
	lda (GfxPtr),y
	sta DF7WRITE
	dey
	bpl .gfx
	SOUND
	ldy #hero_P0_HEIGHT-1
	lda System
	bne .pal
.ntsc
	lda (ColPtr),y          ; NTSC: as they are
	sta DF7WRITE
	dey
	bpl .ntsc
	SOUND
	rts
.pal
	lda (ColPtr),y
	lsr
	tax
	lda NtscToPal,x
	sta DF7WRITE
	dey
	bpl .pal
	SOUND
	rts

; The animations by number (Anim): their tables come from hero.gfx.asm.
AnimFrameLo     .byte <hero_idle_FRAME, <hero_walk_FRAME
AnimFrameHi     .byte >hero_idle_FRAME, >hero_walk_FRAME
AnimTicksLo     .byte <hero_idle_TICKS, <hero_walk_TICKS
AnimTicksHi     .byte >hero_idle_TICKS, >hero_walk_TICKS
AnimSteps       .byte hero_idle_STEPS, hero_walk_STEPS
AnimLoop        .byte hero_idle_LOOP, hero_walk_LOOP

; ---- ConvertColour: A = an NTSC colour; returns the colour for the TV system.
; PAL and PAL60 have other hues and lights: NtscToPal gives the PAL colour that
; looks closest to each NTSC one. Keeps X and Y.
ConvertColour SUBROUTINE
	stx ColourKeep
	ldx System
	beq .done               ; NTSC: as it is
	lsr                     ; one byte for each colour (bit 0 is not used)
	tax
	lda NtscToPal,x
.done
	ldx ColourKeep
	rts

; The closest PAL colour to each NTSC one, $00 to $FE in steps of 2: worked
; out from Stella's NTSC and PAL palettes, as the shortest distance in CIELAB
; (the colour space closest to how the eye tells colours apart). PAL has no
; match for some NTSC colours, like the brightest yellows: the closest one is
; a little off. Change an entry to taste: the conversion is all here.
NtscToPal
	.byte $00,$04,$06,$08,$0A,$0A,$0C,$0E  ; $00: grey
	.byte $22,$24,$24,$28,$28,$28,$2A,$2C  ; $10: gold
	.byte $42,$44,$46,$46,$46,$28,$2A,$2A  ; $20: orange
	.byte $42,$42,$44,$46,$46,$48,$48,$4A  ; $30: red orange
	.byte $42,$62,$62,$64,$64,$66,$68,$68  ; $40: pink
	.byte $82,$82,$84,$84,$86,$86,$88,$88  ; $50: purple
	.byte $A0,$A2,$A4,$A6,$A6,$A8,$A8,$A8  ; $60: blue purple
	.byte $C0,$C0,$D4,$D4,$D6,$C8,$C8,$CA  ; $70: blue
	.byte $D0,$D2,$D4,$D4,$D6,$D6,$D8,$D8  ; $80: blue
	.byte $B2,$B2,$B4,$B6,$B6,$B8,$BA,$BA  ; $90: light blue
	.byte $90,$92,$92,$94,$96,$98,$9A,$9A  ; $A0: turquoise
	.byte $70,$72,$72,$76,$78,$78,$7A,$7C  ; $B0: green blue
	.byte $30,$50,$32,$56,$58,$58,$5A,$5A  ; $C0: green
	.byte $30,$50,$32,$56,$58,$3A,$3C,$3C  ; $D0: yellow green
	.byte $20,$22,$24,$24,$2A,$2A,$2A,$2C  ; $E0: orange green
	.byte $20,$22,$24,$24,$28,$2A,$2A,$2C  ; $F0: light orange

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
.no
	clc
	rts

; ---- Wait12: jsr and rts take 12 cycles: a short wait in a kernel.
Wait12
	rts

; ---- BeamOn: the first visible line, with fast fetch on, for the kernels.
BeamOn
	sta WSYNC
	SOUND
	lda #0
	sta VBLANK              ; beam on
	sta FASTFETCH           ; fast fetch on: lda #n (n < $28) reads register n
	rts

; ---- BlankLines: X = lines with nothing drawn, and the sound on each.
BlankLines SUBROUTINE
.line
	sta WSYNC
	lda AMPLITUDE           ; (absolute: with fast fetch on or off)
	sta AUDV0
	dex
	bne .line
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

	include "hero.gfx.asm"
	include "enemy.gfx.asm"
	include "room.gfx.asm"
	include "title.gfx.asm"

; the music player, in bank 4
	IF * > TRAMPOLINES
	ERR ; bank 5 is full
	ENDIF
	ORG $6000+TRAMPOLINES-$F000
	RORG TRAMPOLINES
	CALL_BANK4 PlayerInit
	CALL_BANK4 PlayerFrame
	CALL_BANK4 PlayerEffect

	ORG $6FF0
	RORG $FFF0
	sta BANK5               ; as in the other banks
	jmp Reset
	ORG $6FFC
	RORG $FFFC
	.word $FFF0, $FFF0

; ---- Display data (4K): what the fetchers read. Its labels are offsets in it.
	ORG $7000
	RORG $0000
MUSIC_PART SET 2
	include "theme.music.asm"       ; the waveforms

; Written at power on by CopyTables, top row first, in CopyList's order.
CartPF0D  ds cart_ROWS          ; the title's cartridge
CartPF1D  ds cart_ROWS
CartPF2D  ds cart_ROWS
CartPF0RD ds cart_ROWS
CartPF1RD ds cart_ROWS
CartPF2RD ds cart_ROWS
TextPF0D  ds TEXT_ROWS*4        ; the menu's texts: PLAY, NTSC, PAL60, PAL
TextPF1D  ds TEXT_ROWS*4
TextPF2D  ds TEXT_ROWS*4
TextPF0RD ds TEXT_ROWS*4
TextPF1RD ds TEXT_ROWS*4
TextPF2RD ds TEXT_ROWS*4
RoomPF0D  ds room_ROWS          ; the room
RoomPF1D  ds room_ROWS
RoomPF2D  ds room_ROWS
; Written as it runs.
CartColD  ds cart_ROWS          ; the cartridge's row colours, for the TV system
HeroGfxBuf ds hero_P0_HEIGHT    ; the hero's frame (Animate)...
HeroColBuf ds hero_P0_HEIGHT    ; ...and its colours
EnemyBuf  ds enemy_P1_HEIGHT    ; the shown enemy's frame (GameLogic)
; Fixed.
ShotBuf   .byte 2               ; ENABL on the shot's line pair
CartSidesD                      ; ENAM0 and ENAM1 for each row of the cartridge:
	.byte 0                 ; off on the top lip, on below it
	ds cart_ROWS-1, 2
	IF HeroGfxBuf < ROOM_PAIRS
	ERR ; the game's buffers must be at least ROOM_PAIRS bytes into the display data
	ENDIF

; THE GAME, 48 pixels wide: six columns of 8, two lines per row, top first.
TheGame0D
	.byte %11111010  ; XXXXX.X.
	.byte %11111010  ; XXXXX.X.
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
	.byte %00100010  ; ..X...X.
	.byte %00100010  ; ..X...X.
TheGame1D
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
TheGame2D
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
TheGame3D
	.byte %01111001  ; .XXXX..X
	.byte %01111001  ; .XXXX..X
	.byte %10000010  ; X.....X.
	.byte %10000010  ; X.....X.
	.byte %10000010  ; X.....X.
	.byte %10000010  ; X.....X.
	.byte %10011011  ; X..XX.XX
	.byte %10011011  ; X..XX.XX
	.byte %10001010  ; X...X.X.
	.byte %10001010  ; X...X.X.
	.byte %10001010  ; X...X.X.
	.byte %10001010  ; X...X.X.
	.byte %01111010  ; .XXXX.X.
	.byte %01111010  ; .XXXX.X.
TheGame4D
	.byte %11001000  ; XX..X...
	.byte %11001000  ; XX..X...
	.byte %00101101  ; ..X.XX.X
	.byte %00101101  ; ..X.XX.X
	.byte %00101010  ; ..X.X.X.
	.byte %00101010  ; ..X.X.X.
	.byte %11101010  ; XXX.X.X.
	.byte %11101010  ; XXX.X.X.
	.byte %00101000  ; ..X.X...
	.byte %00101000  ; ..X.X...
	.byte %00101000  ; ..X.X...
	.byte %00101000  ; ..X.X...
	.byte %00101000  ; ..X.X...
	.byte %00101000  ; ..X.X...
TheGame5D
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
	IF * > $1000
	ERR ; the display data is full
	ENDIF

; ---- Frequency table (1K) ------------------------------------------------------
	ORG $8000
	RORG $1000
MUSIC_PART SET 3
	include "theme.music.asm"
	ORG $83FF
	.byte 0                 ; the image ends at 32K
