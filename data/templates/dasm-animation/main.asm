; {{NAME}} - made with VCSIDE
; Template: SPDX-License-Identifier: CC0-1.0 (public domain; your game, your licence)
; 4K cartridge, {{TV}} timing. Build: F7 (or Build > Build). Run: F5.
;
; Example: an animated sprite. The hero is drawn in hero.vcsgfx (open it from
; the project panel); saving it writes hero.gfx.asm, included below, with
; its frames and two animations: idle (it blinks) and walk. The joystick moves
; it left and right. The kernel draws it with two scanlines per row, reading
; the frame through a pointer, and the vertical blank plays the animations
; with their tables: for each step, the frame (_FRAME) and how many frames
; it lasts (_TICKS).
;
; Music and sound: theme.vcsmusic (open it from the project panel) is the song
; and its two sound effects; saving it writes theme.music.asm, the player and
; the song's tables, included below. MusicInit starts the song, MusicFrame
; plays it once per frame, and MusicEffect plays an effect: jump on the fire
; button, bump when the hero reaches an edge. While an effect sounds, the
; song's notes on its channel wait.

	processor 6502
	include "2600.h"

; ---- TV timing (scanlines) -------------------------------------------------
{{TVDEFS}}
PAIRS          = VISIBLE_LINES / 2  ; the kernel draws two lines per pass

; ---- RAM -------------------------------------------------------------------
	SEG.U Variables
	ORG $80
HeroX   ds 1            ; colour clocks from the left
HeroY   ds 1            ; line pair of the hero's bottom row (0 = the bottom one)
Anim    ds 1            ; 0 idle, 1 walk (the order of the tables below)
Step    ds 1            ; step of the animation
Timer   ds 1            ; frames into the step
GfxPtr  ds 2            ; the frame being shown
ColPtr  ds 2            ; its colours
CurGfx  ds 1            ; what the next line pair writes
CurCol  ds 1
Ptr     ds 2            ; scratch pointer
Button  ds 1            ; the fire button on the last frame (0 = down)
Bump    ds 1            ; not 0: the hero has just reached an edge
MusicRam ds 19          ; the music player's
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
	lda #76
	sta HeroX
	lda #30
	sta HeroY
	lda #$90                ; dark blue sky
	sta COLUBK
	lda #$80
	sta Button              ; as if up: holding it at power on is no jump
	jsr MusicInit           ; the song from its start

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

	jsr Joystick
	jsr Animate
	jsr Sounds
	jsr MusicFrame          ; the music's writes for this frame
	lda HeroX
	ldx #0                  ; player 0
	jsr PosObject
	sta WSYNC
	sta HMOVE

.vblank lda INTIM
	bne .vblank
	sta WSYNC
	sta VBLANK              ; A = 0: beam on

; ---- Kernel: a line pair per pass. The first line writes what the second
; one worked out on the pass before.
	ldx #PAIRS
.pair   sta WSYNC
	lda CurGfx              ; 3
	sta GRP0                ; 3
	lda CurCol              ; 3
	sta COLUP0              ; 3
	txa                     ; 2  the hero's row on this pair (bottom = 0)
	sec                     ; 2
	sbc HeroY               ; 3
	cmp #hero_P0_HEIGHT     ; 2
	bcs .none               ; 2/3
	tay                     ; 2
	lda (GfxPtr),y          ; 5  the tables are bottom row first
	sta CurGfx              ; 3
	lda (ColPtr),y          ; 5
	sta CurCol              ; 3
	jmp .next               ; 3
.none   lda #0
	sta CurGfx
.next   sta WSYNC
	dex
	bne .pair

	lda #0
	sta GRP0
	lda #2
	sta VBLANK
	lda #(OVERSCAN_LINES*76-14)/64
	sta TIM64T
.over   lda INTIM
	bne .over
	jmp StartFrame

; ---- Joystick: left and right move the hero and choose its animation -------
Joystick
	ldy #0                  ; idle unless a direction is held
	lda SWCHA
	bmi .noRight            ; D7 = 0: right
	ldy #1
	lda #0
	sta REFP0               ; facing right
	lda HeroX
	cmp #152
	bcs .noRight
	inc HeroX
	lda HeroX
	cmp #152
	bne .noRight
	sta Bump                ; just got to the right edge
.noRight
	lda SWCHA
	asl
	bmi .noLeft             ; D6 = 0: left
	ldy #1
	lda #8
	sta REFP0               ; facing left
	lda HeroX
	beq .noLeft
	dec HeroX
	bne .noLeft
	inc Bump                ; just got to the left edge
.noLeft
	cpy Anim
	beq .same
	sty Anim                ; another animation: from its first step
	lda #0
	sta Step
	sta Timer
.same   rts

; ---- Sounds: the effects, when the button goes down and at the edges -------
Sounds
	lda INPT4
	and #$80                ; D7 = 0: the button is down
	cmp Button              ; carry clear: down now, up before
	sta Button
	bcs .noJump
	lda #MUSIC_EFFECT_JUMP
	jsr MusicEffect
.noJump lda Bump
	beq .done
	lda #0
	sta Bump
	lda #MUSIC_EFFECT_BUMP
	jsr MusicEffect
.done   rts

; ---- Animate: advance the current animation and point at its frame ----------
Animate
	ldx Anim
	lda AnimTicksLo,x
	sta Ptr
	lda AnimTicksHi,x
	sta Ptr+1
	ldy Step
	inc Timer
	lda Timer
	cmp (Ptr),y             ; done with this step?
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
	sta Ptr
	lda AnimFrameHi,x
	sta Ptr+1
	ldy Step
	lda (Ptr),y             ; the frame of this step
	tay
	lda hero_P0_FramePtrLo,y
	sta GfxPtr
	lda hero_P0_FramePtrHi,y
	sta GfxPtr+1
	lda hero_P0_ColourPtrLo,y
	sta ColPtr
	lda hero_P0_ColourPtrHi,y
	sta ColPtr+1
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

; The animations by number (Anim): their tables come from hero.gfx.asm.
AnimFrameLo     .byte <hero_idle_FRAME, <hero_walk_FRAME
AnimFrameHi     .byte >hero_idle_FRAME, >hero_walk_FRAME
AnimTicksLo     .byte <hero_idle_TICKS, <hero_walk_TICKS
AnimTicksHi     .byte >hero_idle_TICKS, >hero_walk_TICKS
AnimSteps       .byte hero_idle_STEPS, hero_walk_STEPS
AnimLoop        .byte hero_idle_LOOP, hero_walk_LOOP

	include "hero.gfx.asm"
	include "theme.music.asm"

	ORG $FFFC
	.word Reset
	.word Reset
