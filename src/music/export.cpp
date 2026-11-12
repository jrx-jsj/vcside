// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "export.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "../i18n.h"
#include "player.h"

namespace vcs::music {

// The player, written for VCSIDE and dedicated to the public domain (CC0 1.0), so games
// that use it carry no licence. It follows Player (player.cpp) rule by rule.
static const char* kPlayer = R"PLAYER(
; ---------------------------------------------------------------------------
; VCSIDE music player for the TIA.
; Dedicated to the public domain under CC0 1.0: use it in any game, with no
; licence or credit needed.
;
;   jsr MusicInit       once, to start the song from the beginning
;   jsr MusicFrame      once per frame (in the vertical blank or the overscan)
;   lda #MUSIC_EFFECT_x
;   jsr MusicEffect     a sound effect: the song's notes on its channel wait
;                       until it has ended
;
; RAM: MUSIC_RAM and the MUSIC_RAM_BYTES - 1 bytes after it, in zero page.
; MUSIC_CHANNELS: 2 for the TIA; DPC+ (MUSIC_DPC = 1) has 1 to 3 voices, and the
; player writes their WAVEFORMx and NOTEx instead of AUDCx, AUDFx and AUDVx.
; MUSIC_TIA_EFFECTS (DPC+): sound effects on the TIA's channel 1, which the voices
; leave free; MusicFrame writes their AUDC1, AUDF1 and AUDV1.
; ---------------------------------------------------------------------------

MusicOrder        = MUSIC_RAM       ; order entry of the next row ($FF: stopped)
MusicRow          = MUSIC_RAM+1     ; row in the pattern
MusicWait         = MUSIC_RAM+2     ; frames left in the row
MusicPointer      = MUSIC_RAM+3     ; the next row's data (2 bytes)
MusicVolume0      = MUSIC_RAM+5     ; the envelope or the release (2 bytes)
MusicVolumeIndex0 = MUSIC_RAM+7
MusicPitch0       = MUSIC_RAM+8     ; AUDC and AUDF frame by frame (2 bytes)
MusicPitchIndex0  = MUSIC_RAM+10    ; bit 7: a sound effect has the channel
MusicState0       = MUSIC_RAM+11    ; bit 7 sounding, bit 6 released, 0-5 release
MusicVolume1      = MUSIC_RAM+12
MusicVolumeIndex1 = MUSIC_RAM+14
MusicPitch1       = MUSIC_RAM+15
MusicPitchIndex1  = MUSIC_RAM+17
MusicState1       = MUSIC_RAM+18
  IF MUSIC_CHANNELS > 2
MusicVolume2      = MUSIC_RAM+19
MusicVolumeIndex2 = MUSIC_RAM+21
MusicPitch2       = MUSIC_RAM+22
MusicPitchIndex2  = MUSIC_RAM+24
MusicState2       = MUSIC_RAM+25
  ENDIF
  IF MUSIC_TIA_EFFECTS
MusicTia          = MUSIC_RAM+5+7*MUSIC_CHANNELS  ; the effect's next frame (2 bytes)
MusicTiaLeft      = MusicTia+2      ; its frames left + 1 (0: none)
MUSIC_RAM_BYTES   = 8 + 7 * MUSIC_CHANNELS
  ELSE
MUSIC_RAM_BYTES   = 5 + 7 * MUSIC_CHANNELS
  ENDIF

MusicInit SUBROUTINE
    ldx #MUSIC_RAM_BYTES-1
    lda #0
.clear
    sta MUSIC_RAM,x
    dex
    bpl .clear
    lda MusicOrderLo
    sta MusicPointer
    lda MusicOrderHi
    sta MusicPointer+1
    rts

MusicFrame SUBROUTINE
  IF MUSIC_DPC
    lda AMPLITUDE          ; the kernel's sound goes on while the player works
    sta AUDV0
  ENDIF
  IF MUSIC_TIA_EFFECTS
    jsr MusicTiaFrame
  ENDIF
    lda MusicOrder
    cmp #$FF
    beq .voices
    lda MusicWait
    bne .wait
    lda MusicOrder
    cmp #MUSIC_ORDER_LENGTH
    bcc .row
    ; the song is over: what sounds is released (sound effects go on)
    bit MusicPitchIndex0
    bmi .keep0
    jsr MusicNoteOff0
.keep0
  IF MUSIC_CHANNELS > 1
    bit MusicPitchIndex1
    bmi .keep1
    jsr MusicNoteOff1
.keep1
  ENDIF
  IF MUSIC_CHANNELS > 2
    bit MusicPitchIndex2
    bmi .keep2
    jsr MusicNoteOff2
.keep2
  ENDIF
    lda #$FF
    sta MusicOrder
    bne .voices
.row
    jsr MusicPlayRow
.wait
    dec MusicWait
.voices
  IF MUSIC_CHANNELS > 2
    jsr MusicVoice2
  ENDIF
  IF MUSIC_CHANNELS > 1
    jsr MusicVoice1
  ENDIF
    jmp MusicVoice0

MusicPlayRow SUBROUTINE
    ldy #0
    jsr MusicEvent0
  IF MUSIC_CHANNELS > 1
    jsr MusicEvent1
  ENDIF
  IF MUSIC_CHANNELS > 2
    jsr MusicEvent2
  ENDIF
    tya
    clc
    adc MusicPointer
    sta MusicPointer
    bcc .length
    inc MusicPointer+1
.length
  IF MUSIC_PATTERN_SPEEDS
    ldx MusicOrder              ; each pattern's speed: its even and odd rows' frames
    lda MusicRow
    lsr
    lda MusicOrderSpeed,x
    bcc .evenRow
    lda MusicOrderSwing,x
.evenRow
    tax
  ELSE
    ldx #MUSIC_SPEED
    IF MUSIC_SWING > 0
    lda MusicRow
    lsr
    bcc .even
    ldx #MUSIC_SWING
.even
    ENDIF
  ENDIF
    stx MusicWait
    inc MusicRow
    ldx MusicOrder
    lda MusicRow
    cmp MusicOrderRows,x
    bcc .done
    lda #0
    sta MusicRow
    inx
    cpx #MUSIC_ORDER_LENGTH
    bcc .entry
    ldx #MUSIC_LOOP_TO
.entry
    stx MusicOrder
    cpx #MUSIC_ORDER_LENGTH
    bcs .done
    lda MusicOrderLo,x
    sta MusicPointer
    lda MusicOrderHi,x
    sta MusicPointer+1
.done
    rts

MusicEffect SUBROUTINE
    tay
    ldx MusicEffectSound,y
    lda MusicEffectChannel,y
  IF MUSIC_TIA_EFFECTS
    bpl .voice
    lda MusicTiaLo,x       ; $FF: on the TIA's channel 1; X is its table
    sta MusicTia
    lda MusicTiaHi,x
    sta MusicTia+1
    ldy #0
    lda (MusicTia),y       ; its frames + 1
    sta MusicTiaLeft
    inc MusicTia
    bne .tiaReady
    inc MusicTia+1
.tiaReady
    rts
.voice                     ; (bpl keeps the flags of the channel's lda)
  ENDIF
    bne .channel1
    jsr MusicStart0
    lda #$80
    sta MusicPitchIndex0
    rts
.channel1
  IF MUSIC_CHANNELS > 2
    cmp #2
    beq .channel2
  ENDIF
  IF MUSIC_CHANNELS > 1
    jsr MusicStart1
    lda #$80
    sta MusicPitchIndex1
  ENDIF
    rts
  IF MUSIC_CHANNELS > 2
.channel2
    jsr MusicStart2
    lda #$80
    sta MusicPitchIndex2
    rts
  ENDIF

  IF MUSIC_TIA_EFFECTS
; The sound effect on the TIA's channel 1: this frame's AUDC1, AUDF1 and AUDV1.
MusicTiaFrame SUBROUTINE
    lda MusicTiaLeft
    beq .done              ; none
    dec MusicTiaLeft
    beq .over
    ldy #0
    lda (MusicTia),y
    sta AUDC1
    iny
    lda (MusicTia),y
    sta AUDF1
    iny
    lda (MusicTia),y
    sta AUDV1
    lda MusicTia
    clc
    adc #3
    sta MusicTia
    bcc .done
    inc MusicTia+1
.done
    rts
.over
    lda #0                 ; it is over: silent
    sta AUDV1
    rts
  ENDIF

; One channel's routines; {1} is the channel.
    MAC MUSIC_CHANNEL

; A row's event for the channel: Y is the offset in the row and moves past it.
MusicEvent{1} SUBROUTINE
  IF MUSIC_DPC
    lda AMPLITUDE          ; the kernel's sound goes on while the player works
    sta AUDV0
  ENDIF
    lda (MusicPointer),y
    iny
    tax
    cpx #2
    bcc .read
    cpx #5
    bcs .read
    lda (MusicPointer),y   ; codes 2 to 4 take an argument
    iny
.read
    bit MusicPitchIndex{1}
    bmi .done              ; a sound effect has the channel
    cpx #6
    bcs .sound
    cpx #1
    beq .off
    cpx #2
    beq .volume
    cpx #3
    beq .release
    cpx #4
    beq .offWith
    cpx #5
    beq .silence
.done
    rts
.sound
    txa
    sbc #6
    tax
    jmp MusicStart{1}
.silence
    lda #0
    sta MusicState{1}
    rts
.volume
    bit MusicState{1}
    bvs .done              ; released: the envelope no longer counts
    tax
    lda MusicEnvelopeLo,x
    sta MusicVolume{1}
    lda MusicEnvelopeHi,x
    sta MusicVolume{1}+1
    rts
.release
    tax
    lda MusicState{1}
    and #$C0
    sta MusicState{1}
    txa
    ora MusicState{1}
    sta MusicState{1}
    rts
.offWith
    tax
    lda MusicState{1}
    and #$C0
    sta MusicState{1}
    txa
    ora MusicState{1}
    sta MusicState{1}
.off
    jmp MusicNoteOff{1}

; A sound starts: X is its number.
MusicStart{1} SUBROUTINE
    lda MusicSoundPitchLo,x
    sta MusicPitch{1}
    lda MusicSoundPitchHi,x
    sta MusicPitch{1}+1
    lda MusicSoundRelease,x
    ora #$80
    sta MusicState{1}
    lda MusicSoundEnvelope,x
    tax
    lda MusicEnvelopeLo,x
    sta MusicVolume{1}
    lda MusicEnvelopeHi,x
    sta MusicVolume{1}+1
    lda #0
    sta MusicVolumeIndex{1}
    sta MusicPitchIndex{1}
    rts

; The note is let go: its release plays once.
MusicNoteOff{1} SUBROUTINE
    bit MusicState{1}
    bpl .done              ; not sounding
    bvs .done              ; already released
    lda MusicState{1}
    ora #$40
    sta MusicState{1}
    and #$3F
    tax
    lda MusicReleaseLo,x
    sta MusicVolume{1}
    lda MusicReleaseHi,x
    sta MusicVolume{1}+1
    lda #0
    sta MusicVolumeIndex{1}
.done
    rts

; This frame's AUDC, AUDF and AUDV for the channel (DPC+: its WAVEFORM and NOTE).
MusicVoice{1} SUBROUTINE
  IF MUSIC_DPC
    lda AMPLITUDE          ; the kernel's sound goes on while the player works
    sta AUDV0
  ENDIF
    lda MusicState{1}
    bmi .sounding
.silent
  IF MUSIC_DPC
    lda #MUSIC_WAVE_OFF
    sta WAVEFORM{1}
    lda #0
  ELSE
    lda #0
    sta AUDV{1}
    sta AUDC{1}
    sta AUDF{1}
  ENDIF
    sta MusicPitchIndex{1}  ; and the channel is the song's again
    rts
.stop
    lda #0
    sta MusicState{1}
    beq .silent
.sounding
    ldy #0
    lda MusicVolumeIndex{1}
    cmp (MusicVolume{1}),y  ; past the end?
    bcc .read
    bit MusicState{1}
    bvs .stop               ; the release is over
    iny
    lda (MusicVolume{1}),y  ; the loop
    cmp #$FF
    bne .loop
    dey
    lda (MusicVolume{1}),y  ; no loop: the last value stays
    beq .stop
    sec
    sbc #1
    sta MusicVolumeIndex{1}
    tay
    iny
    iny
    lda (MusicVolume{1}),y
    beq .stop               ; it ends on 0: the note is over
    bne .volume
.loop
    sta MusicVolumeIndex{1}
.read
    lda MusicVolumeIndex{1}
    clc
    adc #2
    tay
    lda (MusicVolume{1}),y
.volume
    inc MusicVolumeIndex{1}
    pha
    lda MusicPitchIndex{1}
    and #$7F
    tax                     ; this frame's step
    ldy #0
    lda (MusicPitch{1}),y
    and #$7F
    sec
    sbc #1                  ; the last step, which stays
    eor MusicPitchIndex{1}
    and #$7F
    beq .held
    inc MusicPitchIndex{1}
.held
    txa
    asl
    tay
    iny
  IF MUSIC_DPC
    pla                     ; the volume: one of the wave's copies (or none)
    tax
    lda MusicDpcLevel,x
    clc
    adc (MusicPitch{1}),y   ; the wave's first entry in MusicWaveSlots
    tax
    lda MusicWaveSlots,x
    sta WAVEFORM{1}
    iny
    lda (MusicPitch{1}),y
    sta NOTE{1}
    rts
  ELSE
    lda (MusicPitch{1}),y
    sta AUDC{1}
    iny
    lda (MusicPitch{1}),y
    sta AUDF{1}
    pla
    tax
    ldy #0
    lda (MusicPitch{1}),y
    bpl .heard
    ldx #0                  ; no allowed AUDC reaches the note: silent
.heard
    stx AUDV{1}
    rts
  ENDIF

    ENDM

    MUSIC_CHANNEL 0
  IF MUSIC_CHANNELS > 1
    MUSIC_CHANNEL 1
  ENDIF
  IF MUSIC_CHANNELS > 2
    MUSIC_CHANNEL 2
  ENDIF
)PLAYER";

// The C player, for songs played as samples on ELF cartridges. CC0 like the 6502 one; it
// follows Player rule by rule and mixes as SampleMixer (player.cpp) does.
static const char* kCPlayer = R"PLAYER(
/* ---------------------------------------------------------------------------
 * VCSIDE music player for ELF cartridges: voices with waveforms, mixed by the ARM
 * into one 4-bit sample per scanline, played by writing AUDV0 with AUDC0 = 0.
 * Dedicated to the public domain under CC0 1.0: use it in any game, with no
 * licence or credit needed.
 *
 *   music_init();         once, to start the song from the beginning
 *   music_effect(n);      a sound effect (MUSIC_EFFECT_x): the song's notes on
 *                         its voice wait until it has ended
 *   music_frame(lines);   once per frame, in the overblank: the frame's song and
 *                         its samples, for a frame of `lines` lines (262 or 312)
 *
 * Then, in the kernel, music_level[n] goes to AUDV0 on its line n (n from 0 to
 * lines - 64), and the 31 bytes of music_nibbles to $E0-$FE of the RIOT's RAM:
 * the overblank program of the ELF loader plays them, one 4-bit level per line,
 * once its "sta $2d" is changed to "sta AUDV0" (see the template's start_6507).
 *
 * CDFJ (MUSIC_CDFJ 1): the cartridge plays the voices, so nothing is mixed. Define,
 * before including this file, what gives a voice its waveform and its note:
 *   MUSIC_CDFJ_WAVE(voice, samples)   the voice's 32 samples changed (its waveform at
 *                                     its volume): copy them to its wave in display RAM
 *   MUSIC_CDFJ_NOTE(voice, frequency, restart)   every frame for a voice that sounds:
 *                                     the frequency for setNote (2^32 x Hz / 20000);
 *                                     restart: a note started (resetWave, if wanted)
 * and copy the amplitude stream to AUDV0 on every line, with AUDC0 = 0.
 *
 * Sound effects on the TIA's channel 1 (MUSIC_TIA_EFFECTS): music_frame leaves this
 * frame's AUDC1, AUDF1 and AUDV1 in music_tia[0], [1] and [2], for the game to write
 * once per frame (AUDV1 is 0 once the effect is over); music_tia_left is 0 when none
 * sounds, for a game that also uses channel 1 for itself.
 * ------------------------------------------------------------------------- */

#define MUSIC_OVERBLANK_LINES 63

typedef struct {
    uint16_t volume;        /* the envelope or the release, in music_volume_data */
    uint8_t volume_index;
    uint8_t pitch_index;    /* bit 7: a sound effect has the voice */
    uint16_t pitch;         /* the sound's phase steps, in music_pitch_data */
    uint8_t state;          /* bit 7 sounding, bit 6 released, bits 0-5 the release */
    uint8_t level;          /* this frame's volume, 0-15 */
    uint8_t wave, last_step;
    uint16_t noise;
    uint32_t step, phase;   /* CDFJ: step is the frequency at 20 kHz */
#if MUSIC_CDFJ
    uint16_t cdfj_wave;     /* the samples last given: waveform + 1 << 8 | level (0: silent) */
    uint8_t restart;
#endif
} music_voice;

static music_voice music_voices[MUSIC_VOICES];
static uint8_t music_order, music_row, music_wait;   /* music_order 0xFF: stopped */
static uint16_t music_pointer;                       /* the next row, in music_patterns */
#if MUSIC_CDFJ
#if !defined(MUSIC_CDFJ_WAVE) || !defined(MUSIC_CDFJ_NOTE)
#error "define MUSIC_CDFJ_WAVE(voice, samples) and MUSIC_CDFJ_NOTE(voice, frequency, restart) before including the music"
#endif
static uint8_t music_cdfj_samples[MUSIC_VOICES][32];
#else
uint8_t music_level[312 - MUSIC_OVERBLANK_LINES];
uint8_t music_nibbles[31];
#endif
#if MUSIC_TIA_EFFECTS
uint8_t music_tia[3];               /* AUDC1, AUDF1 and AUDV1 for this frame */
uint8_t music_tia_left;             /* frames left of the effect + 1 (0: none) */
static uint16_t music_tia_at;       /* its next frame, in music_tia_data */
#endif

static void music_start(music_voice *voice, int sound)
{
    voice->pitch = music_sound_pitch[sound];
    voice->state = (uint8_t)(music_sound_release[sound] | 0x80);
    voice->volume = music_envelope_offset[music_sound_envelope[sound]];
    voice->volume_index = 0;
    voice->pitch_index = 0;
    voice->phase = 0;
    voice->noise = 1;
    voice->last_step = 0;
#if MUSIC_CDFJ
    voice->restart = 1;
#endif
}

static void music_note_off(music_voice *voice)
{
    if (!(voice->state & 0x80) || (voice->state & 0x40)) return;
    voice->state |= 0x40;
    voice->volume = music_release_offset[voice->state & 0x3F];
    voice->volume_index = 0;
}

void music_init(void)
{
    for (int i = 0; i < MUSIC_VOICES; i++) {
        music_voices[i].state = 0;
        music_voices[i].pitch_index = 0;
#if MUSIC_CDFJ
        music_voices[i].cdfj_wave = 0xFFFF;             /* the first frame gives every voice its samples */
#endif
    }
    music_order = music_row = music_wait = 0;
    music_pointer = music_order_offset[0];
#if MUSIC_TIA_EFFECTS
    music_tia_left = 0;
    music_tia[0] = music_tia[1] = music_tia[2] = 0;
#endif
}

void music_effect(int effect)
{
#if MUSIC_TIA_EFFECTS
    if (music_effect_voice[effect] == 0xFF) {           /* on the TIA's channel 1 */
        music_tia_at = music_tia_offset[music_effect_sound[effect]];
        music_tia_left = music_tia_data[music_tia_at++];
        return;
    }
#endif
    music_voice *voice = &music_voices[music_effect_voice[effect]];
    music_start(voice, music_effect_sound[effect]);
    voice->pitch_index = 0x80;
}

/* A row's event for a voice, from music_patterns[at]; returns where the next one is. */
static int music_event(music_voice *voice, int at)
{
    int code = music_patterns[at++], argument = 0;
    if (code >= 2 && code <= 4) argument = music_patterns[at++];
    if (voice->pitch_index & 0x80) return at;          /* a sound effect has the voice */
    if (code >= 6) {
        music_start(voice, code - 6);
    } else if (code == 1) {
        music_note_off(voice);
    } else if (code == 2) {
        if (!(voice->state & 0x40)) voice->volume = music_envelope_offset[argument];
    } else if (code == 3 || code == 4) {
        voice->state = (uint8_t)((voice->state & 0xC0) | argument);
        if (code == 4) music_note_off(voice);
    } else if (code == 5) {
        voice->state = 0;
    }
    return at;
}

static void music_play_row(void)
{
    int at = music_pointer;
    for (int i = 0; i < MUSIC_VOICES; i++) at = music_event(&music_voices[i], at);
    music_pointer = (uint16_t)at;
#if MUSIC_PATTERN_SPEEDS
    music_wait = (music_row & 1) ? music_order_swing[music_order] : music_order_speed[music_order];
#else
    music_wait = (MUSIC_SWING > 0 && (music_row & 1)) ? MUSIC_SWING : MUSIC_SPEED;
#endif
    if (++music_row >= music_order_rows[music_order]) {
        music_row = 0;
        if (++music_order >= MUSIC_ORDER_LENGTH) music_order = MUSIC_LOOP_TO;
        if (music_order < MUSIC_ORDER_LENGTH) music_pointer = music_order_offset[music_order];
    }
}

/* This frame's volume and phase step for a voice. */
static void music_voice_frame(music_voice *voice)
{
    if (!(voice->state & 0x80)) {
        voice->level = 0;
        voice->pitch_index = 0;                         /* the voice is the song's again */
        return;
    }
    const uint8_t *volume = music_volume_data + voice->volume;
    int index = voice->volume_index, value;
    if (index >= volume[0]) {
        if (voice->state & 0x40) goto stop;             /* the release is over */
        if (volume[1] != 0xFF) {
            index = volume[1];                          /* the loop */
        } else {
            if (volume[0] == 0) goto stop;
            index = volume[0] - 1;                      /* the last value stays... */
            if (volume[2 + index] == 0) goto stop;      /* ...unless it is 0: the note is over */
        }
    }
    value = volume[2 + index];
    voice->volume_index = (uint8_t)(index + 1);
    {
        const uint32_t *pitch = music_pitch_data + voice->pitch;
        int length = (int)(pitch[0] & 0xFF), step = voice->pitch_index & 0x7F;
        if (step != length - 1) voice->pitch_index++;
        voice->step = pitch[1 + step];
        voice->wave = (uint8_t)(pitch[0] >> 8);
    }
    voice->level = (uint8_t)value;
    return;
stop:
    voice->state = 0;
    voice->level = 0;
    voice->pitch_index = 0;
}

void music_frame(int lines)
{
    if (music_order != 0xFF) {
        if (music_wait == 0) {
            if (music_order >= MUSIC_ORDER_LENGTH) {    /* the song is over: what sounds is released */
                for (int i = 0; i < MUSIC_VOICES; i++)
                    if (!(music_voices[i].pitch_index & 0x80)) music_note_off(&music_voices[i]);
                music_order = 0xFF;
                goto voices;
            }
            music_play_row();
        }
        music_wait--;
    }
voices:
    for (int i = 0; i < MUSIC_VOICES; i++) music_voice_frame(&music_voices[i]);
#if MUSIC_TIA_EFFECTS
    if (music_tia_left && --music_tia_left) {           /* the effect's frame on the TIA's channel 1 */
        music_tia[0] = music_tia_data[music_tia_at];
        music_tia[1] = music_tia_data[music_tia_at + 1];
        music_tia[2] = music_tia_data[music_tia_at + 2];
        music_tia_at += 3;
    } else {
        music_tia[2] = 0;                               /* none, or it is over: silent */
    }
#endif
#if MUSIC_CDFJ
    /* each voice's waveform at one of the levels the cartridge's sum allows */
    (void)lines;
    for (int i = 0; i < MUSIC_VOICES; i++) {
        music_voice *voice = &music_voices[i];
        int level = (voice->state & 0x80) ? music_cdfj_level[voice->level] : 0;
        uint16_t samples_key = (uint16_t)(level ? (voice->wave + 1) << 8 | level : 0);
        if (samples_key != voice->cdfj_wave) {
            voice->cdfj_wave = samples_key;
            /* each sample: wave (0-15) x level / 15, rounded; x 547 >> 14 is / 30 for these
             * values, and costs no division (the ARM7 has no divide instruction) */
            for (int j = 0; j < 32; j++)
                music_cdfj_samples[i][j] = (uint8_t)(level ? ((2 * music_waves[voice->wave][j] * level + 15) * 547) >> 14 : 0);
            MUSIC_CDFJ_WAVE(i, music_cdfj_samples[i]);
        }
        if (level) {
            MUSIC_CDFJ_NOTE(i, voice->step, voice->restart);
            voice->restart = 0;
        }
    }
#else
    /* the samples: each voice adds its waveform's level times its volume; the sum, in
     * quarters of a level, goes through music_dac (the TIA's output is not proportional
     * to AUDV). The first lines - 63 are the kernel's, the last 63 the overblank's. */
    static uint8_t overblank[MUSIC_OVERBLANK_LINES];
    int kernel = lines - MUSIC_OVERBLANK_LINES;
    if (kernel > (int)sizeof music_level) kernel = (int)sizeof music_level;
    for (int line = 0; line < kernel + MUSIC_OVERBLANK_LINES; line++) {
        uint32_t sum = 0;
        for (int i = 0; i < MUSIC_VOICES; i++) {
            music_voice *voice = &music_voices[i];
            if (!(voice->state & 0x80)) continue;
            voice->phase += voice->step;
            int step = (int)(voice->phase >> 27), level;
            uint16_t mask = music_wave_noise[voice->wave];
            if (mask) {                                 /* noise: the register moves each 32nd of a cycle */
                if (step != voice->last_step) {
                    voice->last_step = (uint8_t)step;
                    voice->noise = (uint16_t)((voice->noise >> 1) ^ ((voice->noise & 1) ? mask : 0));
                }
                level = (voice->noise & 1) ? 15 : 0;
            } else {
                level = music_waves[voice->wave][step];
            }
            sum += (uint32_t)(level * voice->level);
        }
        uint32_t quarters = (sum * MUSIC_MIX + 32768) >> 16;
        uint8_t out = music_dac[quarters > 63 ? 63 : quarters];
        if (line < kernel) music_level[line] = out;
        else overblank[line - kernel] = out;
    }
    /* The overblank program writes nibble k - 1 on its line k (line 0 repeats nibble 0,
     * so nibble 0 is the mean of the first two lines); even nibbles are the low ones. */
    music_nibbles[0] = (uint8_t)(((overblank[0] + overblank[1] + 1) >> 1) | overblank[2] << 4);
    for (int j = 1; j < 31; j++) music_nibbles[j] = (uint8_t)(overblank[2 * j + 1] | overblank[2 * j + 2] << 4);
#endif
}
)PLAYER";

bool exportTargetOf(const std::string& name, ExportTarget* target) {
  static const std::pair<const char*, ExportTarget> names[] = {
      {"dasm", ExportTarget::Dasm}, {"bb", ExportTarget::BatariBasic}, {"c", ExportTarget::C}, {"dpc", ExportTarget::DpcPlus}, {"cdfj", ExportTarget::Cdfj}};
  for (auto& [text, value] : names)
    if (name == text) {
      if (target) *target = value;
      return true;
    }
  return false;
}

const char* exportExtension(ExportTarget target) {
  return target == ExportTarget::C || target == ExportTarget::Cdfj ? ".music.h" : ".music.asm";
}

std::string effectSymbol(const std::string& name) {
  std::string symbol = "MUSIC_EFFECT_";
  for (char letter : name) {
    if (std::isalnum((unsigned char)letter)) symbol += (char)std::toupper((unsigned char)letter);
    else if (symbol.back() != '_') symbol += '_';
  }
  while (symbol.size() > 13 && symbol.back() == '_') symbol.pop_back();
  return symbol;
}

namespace {
// Between the entries of a .byte list: 8 to a line (DASM's lines are short).
std::string listSeparator(size_t index) { return index == 0 ? "    .byte " : index % 8 ? "," : "\n    .byte "; }
}  // namespace

int dpcMaxLevel(int voices) { return 15 / std::clamp(voices, 1, 3); }

int dpcLevel(int volume, int voices, int gain) {
  int most = dpcMaxLevel(voices);
  return std::clamp((int)std::lround(std::clamp(volume, 0, 15) * most * gain / (15.0 * 16)), 0, most);
}

std::array<uint8_t, 32> dpcWave(const Instrument& instrument) {
  if (instrument.wave != Waveform::Noise) return waveLevels(instrument);
  std::array<uint8_t, 32> levels{};
  int noise = 1, mask = noiseMask(instrument.noiseBits);
  for (auto& level : levels) {
    level = (noise & 1) ? 15 : 0;
    noise = (noise >> 1) ^ ((noise & 1) ? mask : 0);
  }
  return levels;
}

uint32_t dpcFrequency(uint32_t stepPerLine, double audioClock) {
  return (uint32_t)std::min(4294967295.0, std::round(stepPerLine * (audioClock / 2) / 20000.0));
}

namespace {

// Tables, each entry once however many times it is used.
template <typename Key>
struct Pool {
  std::vector<Key> entries;
  std::map<Key, int> index;
  int add(const Key& key) {
    auto found = index.find(key);
    if (found != index.end()) return found->second;
    entries.push_back(key);
    return index[key] = (int)entries.size() - 1;
  }
};

// The codes of a row event (player above): 0 nothing, 1 note off, 2 volume envelope
// (+ envelope), 3 release (+ release), 4 note off with release (+ release), 5 silence,
// 6 and up a sound.
struct Event {
  int code = 0, argument = -1;
  bool operator==(const Event& other) const { return code == other.code && argument == other.argument; }
};

struct Builder {
  Builder(const Song& songToExport, double audioClock, bool forDpc = false) : song(songToExport), clock(audioClock), dpc(forDpc) {}
  const Song& song;
  double clock;
  bool dpc;  // DPC+: pitch pairs are (the wave's first slot entry, the frequency's index)
  Pool<long long> frequencies;  // DPC+: index 1 and up (0 is silence)
  std::vector<std::string> problems;
  std::set<std::string> said;
  Pool<std::pair<std::vector<int>, int>> envelopes;  // values, loop
  Pool<std::vector<int>> releases;
  Pool<std::vector<int>> pitches;                     // length (+$80 muted), then AUDC and AUDF pairs
                                                      // samples: length, waveform, then phase steps
  Pool<std::vector<int>> waves;                       // samples: noise mask (0: none), 32 levels
  Pool<std::tuple<int, int, int>> sounds;             // envelope, pitch, release

  void problem(const std::string& text) {
    if (said.insert(text).second) problems.push_back(text);
  }
  bool valid(int instrument) const { return instrument >= 0 && instrument < (int)song.instruments.size(); }

  int envelope(int instrument, int variantIndex) {
    const Instrument& source = song.instruments[instrument];
    const Envelope& chosen = source.volumes[variantIndex >= 0 && variantIndex < (int)source.volumes.size() ? variantIndex : 0];
    std::vector<int> values(chosen.values.begin(), chosen.values.begin() + std::min<size_t>(chosen.values.size(), 250));
    int loop = chosen.loop >= 0 && chosen.loop < (int)values.size() ? chosen.loop : -1;
    return envelopes.add({values, loop});
  }
  int release(int instrument, int variantIndex) {
    const Instrument& source = song.instruments[instrument];
    const std::vector<int>& chosen = source.releases[variantIndex >= 0 && variantIndex < (int)source.releases.size() ? variantIndex : 0];
    return releases.add(std::vector<int>(chosen.begin(), chosen.begin() + std::min<size_t>(chosen.size(), 250)));
  }
  int pitch(int instrument, int note) {
    const Instrument& source = song.instruments[instrument];
    std::vector<int> bytes;
    bool muted = false;
    std::vector<std::pair<int, int>> steps;
    if (song.samples()) {
      std::vector<uint32_t> phaseSteps = song.phaseSteps(note, source, clock);
      if (phaseSteps.size() > 120) {
        problem(F("{0}: only the first 120 frames of its pitch are exported", {source.name}));
        phaseSteps.resize(120);
      }
      if (dpc) {
        auto levels = dpcWave(source);
        int wave = waves.add(std::vector<int>(levels.begin(), levels.end()));
        bytes.push_back((int)phaseSteps.size());
        for (uint32_t step : phaseSteps)
          bytes.push_back((wave * (dpcMaxLevel(song.channelCount()) + 1)) & 0xFF), bytes.push_back((1 + frequencies.add(dpcFrequency(step, clock))) & 0xFF);
        return pitches.add(bytes);
      }
      std::vector<int> wave = {source.wave == Waveform::Noise ? noiseMask(source.noiseBits) : 0};
      auto levels = waveLevels(source);
      wave.insert(wave.end(), levels.begin(), levels.end());
      bytes = {(int)phaseSteps.size(), waves.add(wave)};
      for (uint32_t step : phaseSteps) bytes.push_back((int)step);
      return pitches.add(bytes);
    }
    if (source.drum) {
      size_t length = std::max<size_t>({source.drumControl.size(), source.drumFrequency.size(), 1});
      for (size_t index = 0; index < length; index++) {
        int control = source.drumControl.empty() ? 8 : source.drumControl[std::min(index, source.drumControl.size() - 1)];
        int frequency = source.drumFrequency.empty() ? 0 : source.drumFrequency[std::min(index, source.drumFrequency.size() - 1)];
        steps.push_back({control, frequency});
      }
    } else {
      std::vector<PitchStep> sequence = song.pitchSequence(note, source, clock);
      muted = !sequence[0].playable;
      if (muted) problem(F("{0} with {1}: no allowed AUDC reaches it, so it is silent", {noteName(note + song.tuning.transpose), source.name}));
      for (auto& step : sequence) steps.push_back({step.control, step.frequency});
    }
    if (steps.size() > 120) {
      problem(F("{0}: only the first 120 frames of its pitch are exported", {source.name}));
      steps.resize(120);
    }
    bytes.push_back((int)steps.size() | (muted ? 0x80 : 0));
    for (auto& [control, frequency] : steps) bytes.push_back(control & 15), bytes.push_back(frequency & 31);
    return pitches.add(bytes);
  }
  int sound(int instrument, int note, int volumeVariant, int releaseVariant) {
    return sounds.add({envelope(instrument, volumeVariant), pitch(instrument, note), release(instrument, releaseVariant)});
  }
};

std::string byteList(const std::vector<int>& values) {
  std::string text;
  for (size_t index = 0; index < values.size(); index++) {
    if (index % 16 == 0) text += index ? "\n    .byte " : "    .byte ";
    else text += ",";
    text += std::to_string(values[index] & 0xff);
  }
  return text.empty() ? "" : text + "\n";
}

}  // namespace

static std::string cList(const std::vector<long long>& values, const char* indent = "    ") {
  std::string text;
  for (size_t index = 0; index < values.size(); index++) {
    if (index % 12 == 0) text += index ? ",\n" + std::string(indent) : std::string(indent);
    else text += ", ";
    text += std::to_string(values[index]);
  }
  return text + "\n";
}

// The song as C data (no pointers: the tables stay in flash), then the C player.
static ExportResult writeC(const Song& song, Builder& builder, std::map<std::tuple<int, int, int>, Event>& events,
                           const std::vector<std::pair<std::string, std::pair<int, int>>>& effects, const std::vector<std::vector<TiaFrame>>& tiaTables,
                           const ExportOptions& options, ExportResult& result) {
  std::ostringstream out;
  int dataBytes = 0;
  int channels = song.channelCount();
  const bool cdfj = options.target == ExportTarget::Cdfj;
  out << "/* " << song.name << " - music for an ELF cartridge of the Atari 2600, made with VCSIDE's music editor.\n";
  out << " * Generated from the .vcsmusic file: change that one, not this. Tuned for " << options.tv << ".\n";
  out << " * Include it once, in the C file that calls the player. */\n";
  out << "#ifndef MUSIC_H_INCLUDED\n#define MUSIC_H_INCLUDED\n#include <stdint.h>\n\n";
  out << "#define MUSIC_VOICES " << channels << "\n";
  out << "#define MUSIC_CDFJ " << (cdfj ? 1 : 0) << "  /* the cartridge plays the voices (see the player below) */\n";
  out << "#define MUSIC_SPEED " << std::clamp(song.speed, 1, 255) << "\n";
  out << "#define MUSIC_SWING " << std::clamp(song.swing, 0, 255) << "\n";
  out << "#define MUSIC_PATTERN_SPEEDS " << (song.patternSpeeds() ? 1 : 0) << "  /* patterns with speeds of their own */\n";
  out << "#define MUSIC_ORDER_LENGTH " << song.order.size() << "\n";
  out << "#define MUSIC_LOOP_TO " << (song.loopTo >= 0 ? std::to_string(song.loopTo) : "MUSIC_ORDER_LENGTH  /* stops at the end */") << "\n";
  out << "#define MUSIC_MIX " << mixFactor(song.gain, channels) << "u  /* the sum of the voices to quarters of a level, in 1/65536 */\n";
  out << "#define MUSIC_TIA_EFFECTS " << tiaTables.size() << "  /* sound effects on the TIA's channel 1 (music_tia) */\n";
  for (size_t index = 0; index < effects.size(); index++) out << "#define " << effects[index].first << " " << index << "\n";
  // patterns, one after another, and where each order entry starts
  std::vector<long long> patternBytes, orderOffset, orderRows;
  std::map<int, int> patternAt;
  for (int patternIndex : song.order) {
    if (patternIndex < 0 || patternIndex >= (int)song.patterns.size() || patternAt.count(patternIndex)) continue;
    patternAt[patternIndex] = (int)patternBytes.size();
    const Pattern& pattern = song.patterns[patternIndex];
    for (int row = 0; row < pattern.rows; row++)
      for (int channel = 0; channel < channels; channel++) {
        Event event = events[std::make_tuple(patternIndex, channel, row)];
        patternBytes.push_back(event.code);
        if (event.argument >= 0) patternBytes.push_back(event.argument);
      }
  }
  for (int patternIndex : song.order) {
    bool valid = patternAt.count(patternIndex) > 0;
    orderOffset.push_back(valid ? patternAt[patternIndex] : 0);
    orderRows.push_back(valid ? song.patterns[patternIndex].rows : 1);
  }
  if (patternBytes.size() > 65535) {
    result.problems.push_back(T("The song is too long for the C player (more than 64 KB of patterns)."));
    return result;
  }
  if (patternBytes.empty()) patternBytes.push_back(0);
  out << "\nstatic const uint8_t music_patterns[] = {\n" << cList(patternBytes) << "};\n";
  out << "static const uint16_t music_order_offset[] = {\n" << cList(orderOffset) << "};\n";
  out << "static const uint8_t music_order_rows[] = {\n" << cList(orderRows) << "};\n";
  if (song.patternSpeeds()) {
    std::vector<long long> evenFrames, oddFrames;
    for (int patternIndex : song.order) {
      bool valid = patternIndex >= 0 && patternIndex < (int)song.patterns.size();
      evenFrames.push_back(std::clamp(valid ? song.rowFrames(song.patterns[patternIndex], 0) : song.speed, 1, 255));
      oddFrames.push_back(std::clamp(valid ? song.rowFrames(song.patterns[patternIndex], 1) : song.rowFrames(1), 1, 255));
    }
    out << "static const uint8_t music_order_speed[] = {\n" << cList(evenFrames) << "};\n";
    out << "static const uint8_t music_order_swing[] = {\n" << cList(oddFrames) << "};\n";
    dataBytes += (int)song.order.size() * 2;
  }
  dataBytes += (int)patternBytes.size() + (int)song.order.size() * 3;
  // envelopes and releases in one table: length, loop (0xFF none), values
  std::vector<long long> volumeData, envelopeOffset, releaseOffset;
  for (auto& [values, loop] : builder.envelopes.entries) {
    envelopeOffset.push_back((long long)volumeData.size());
    volumeData.push_back((long long)values.size()), volumeData.push_back(loop >= 0 ? loop : 0xff);
    volumeData.insert(volumeData.end(), values.begin(), values.end());
  }
  for (auto& values : builder.releases.entries) {
    releaseOffset.push_back((long long)volumeData.size());
    volumeData.push_back((long long)values.size()), volumeData.push_back(0xff);
    volumeData.insert(volumeData.end(), values.begin(), values.end());
  }
  for (auto* list : {&volumeData, &envelopeOffset, &releaseOffset})
    if (list->empty()) list->push_back(0);
  out << "static const uint8_t music_volume_data[] = {\n" << cList(volumeData) << "};\n";
  out << "static const uint16_t music_envelope_offset[] = {\n" << cList(envelopeOffset) << "};\n";
  out << "static const uint16_t music_release_offset[] = {\n" << cList(releaseOffset) << "};\n";
  dataBytes += (int)volumeData.size() + (int)(envelopeOffset.size() + releaseOffset.size()) * 2;
  // phase steps: a word with the length and the waveform (<< 8), then a step per frame
  std::vector<long long> pitchData, pitchAt;
  for (auto& entry : builder.pitches.entries) {
    pitchAt.push_back((long long)pitchData.size());
    pitchData.push_back((long long)entry[0] | (long long)entry[1] << 8);
    for (size_t index = 2; index < entry.size(); index++)
      pitchData.push_back(cdfj ? (long long)dpcFrequency((uint32_t)entry[index], builder.clock) : (long long)(uint32_t)entry[index]);
  }
  if (pitchData.empty()) pitchData.push_back(1), pitchData.push_back(0), pitchAt.push_back(0);
  std::string pitchText;
  for (size_t index = 0; index < pitchData.size(); index++) {
    if (index % 6 == 0) pitchText += index ? ",\n    " : "    ";
    else pitchText += ", ";
    char word[16];
    snprintf(word, sizeof word, "0x%08llXu", pitchData[index]);
    pitchText += word;
  }
  out << "static const uint32_t music_pitch_data[] = {\n" << pitchText << "\n};\n";
  dataBytes += (int)pitchData.size() * 4;
  // sounds and effects
  std::vector<long long> soundEnvelope, soundRelease, soundPitch, effectSound, effectVoice;
  for (auto& [envelopeIndex, pitchIndex, releaseIndex] : builder.sounds.entries)
    soundEnvelope.push_back(envelopeIndex), soundRelease.push_back(releaseIndex), soundPitch.push_back(pitchAt[pitchIndex]);
  for (auto& effect : effects) effectSound.push_back(effect.second.first), effectVoice.push_back(effect.second.second);
  for (auto* list : {&soundEnvelope, &soundRelease, &soundPitch, &effectSound, &effectVoice})
    if (list->empty()) list->push_back(0);
  out << "static const uint8_t music_sound_envelope[] = {\n" << cList(soundEnvelope) << "};\n";
  out << "static const uint8_t music_sound_release[] = {\n" << cList(soundRelease) << "};\n";
  out << "static const uint16_t music_sound_pitch[] = {\n" << cList(soundPitch) << "};\n";
  out << "static const uint8_t music_effect_sound[] = {\n" << cList(effectSound) << "};\n";
  out << "static const uint8_t music_effect_voice[] = {\n" << cList(effectVoice) << "};\n";
  dataBytes += (int)builder.sounds.entries.size() * 4 + (int)effects.size() * 2;
  // sound effects on the TIA's channel 1: frames + 1, then AUDC1, AUDF1 and AUDV1 for each frame
  if (!tiaTables.empty()) {
    std::vector<long long> offsets, bytes;
    for (auto& frames : tiaTables) {
      offsets.push_back((long long)bytes.size());
      bytes.push_back((long long)frames.size() + 1);
      for (auto& frame : frames) bytes.insert(bytes.end(), {frame.control, frame.frequency, frame.volume});
    }
    out << "static const uint16_t music_tia_offset[] = {\n" << cList(offsets) << "};\n";
    out << "static const uint8_t music_tia_data[] = {\n" << cList(bytes) << "};\n";
    dataBytes += (int)offsets.size() * 2 + (int)bytes.size();
  }
  // waveforms: 32 levels each; noise ones have the register's feedback mask instead
  std::vector<long long> masks;
  out << "static const uint8_t music_waves[][32] = {\n";
  for (auto& wave : builder.waves.entries) {
    std::vector<int> levels(wave.begin() + 1, wave.begin() + 33);
    if (cdfj && wave[0]) {  // CDFJ: noise is 32 steps of the register, repeated
      int noise = 1;
      for (auto& level : levels) {
        level = (noise & 1) ? 15 : 0;
        noise = (noise >> 1) ^ ((noise & 1) ? (int)wave[0] : 0);
      }
    }
    masks.push_back(cdfj ? 0 : wave[0]);
    out << "    {";
    for (int step = 0; step < 32; step++) out << (step ? ", " : "") << levels[step];
    out << "},\n";
  }
  if (builder.waves.entries.empty()) out << "    {0},\n", masks.push_back(0);
  out << "};\nstatic const uint16_t music_wave_noise[] = {\n" << cList(masks) << "};\n";
  dataBytes += (int)builder.waves.entries.size() * 34;
  if (cdfj) {
    std::vector<long long> levels;
    for (int volume = 0; volume < 16; volume++) levels.push_back(dpcLevel(volume, channels, song.gain));
    out << "/* a voice's volume to its level: the cartridge adds the voices, AUDV0 takes 4 bits */\n";
    out << "static const uint8_t music_cdfj_level[16] = {\n" << cList(levels) << "};\n";
    dataBytes += 16;
  } else {
    std::vector<long long> dac(levelTable().begin(), levelTable().end());
    out << "/* quarters of a level (0-63) to the AUDV whose output comes closest */\n";
    out << "static const uint8_t music_dac[64] = {\n" << cList(dac) << "};\n";
    dataBytes += 64;
  }
  out << kCPlayer << "\n#endif\n";
  result.ok = true;
  result.text = out.str();
  result.dataBytes = dataBytes;
  result.sounds = (int)builder.sounds.entries.size();
  result.envelopes = (int)builder.envelopes.entries.size();
  result.releases = (int)builder.releases.entries.size();
  result.pitches = (int)builder.pitches.entries.size();
  return result;
}

ExportResult exportSong(const Song& song, double audioClock, const ExportOptions& options) {
  ExportResult result;
  const bool dpc = options.target == ExportTarget::DpcPlus;
  Builder builder{song, audioClock, dpc};
  if (song.instruments.empty() || song.order.empty()) {
    result.problems.push_back(T("The song has no instruments or no order list."));
    return result;
  }
  // Every cell becomes an event. The event can depend on what played before (the
  // channel's instrument, the instrument of the note that sounds), so the song is walked
  // as it plays: from the start, then again from the loop, until nothing changes.
  std::map<std::tuple<int, int, int>, Event> events;  // pattern, channel, row
  const int channels = song.channelCount();
  int channelInstrument[kMaxChannels] = {}, soundingInstrument[kMaxChannels] = {-1, -1, -1, -1};
  auto walk = [&](int from) {
    bool changed = false;
    for (int entry = from; entry < (int)song.order.size(); entry++) {
      int patternIndex = song.order[entry];
      if (patternIndex < 0 || patternIndex >= (int)song.patterns.size()) continue;
      const Pattern& pattern = song.patterns[patternIndex];
      for (int row = 0; row < pattern.rows; row++)
        for (int channel = 0; channel < channels; channel++) {
          const Cell& cell = pattern.cells[channel][row];
          if (cell.instrument >= 0) channelInstrument[channel] = cell.instrument;
          int sounding = soundingInstrument[channel];
          Event event;
          if (cell.note >= 0) {
            int instrument = channelInstrument[channel];
            soundingInstrument[channel] = instrument;
            if (!builder.valid(instrument)) {
              event.code = 5;
            } else {
              int volumeVariant = cell.effect == kEffectVolume ? cell.effectValue : 0;
              int releaseVariant = cell.effect == kEffectRelease ? cell.effectValue : 0;
              event.code = 6 + builder.sound(instrument, cell.note, volumeVariant, releaseVariant);
            }
          } else if (cell.note == kNoteOff) {
            if (cell.effect == kEffectRelease && builder.valid(sounding)) event = {4, builder.release(sounding, cell.effectValue)};
            else event.code = 1;
          } else if (cell.effect == kEffectVolume && builder.valid(sounding)) {
            event = {2, builder.envelope(sounding, cell.effectValue)};
          } else if (cell.effect == kEffectRelease && builder.valid(sounding)) {
            event = {3, builder.release(sounding, cell.effectValue)};
          }
          auto key = std::make_tuple(patternIndex, channel, row);
          auto found = events.find(key);
          if (found == events.end()) {
            events[key] = event;
            changed = true;
          } else if (!(found->second == event)) {
            std::string name = pattern.name.empty() ? F("pattern {0}", {std::to_string(patternIndex)}) : pattern.name;
            builder.problem(F("{0}, channel {1}, row {2}: it plays differently depending on what came before; give the cell its instrument",
                              {name, std::to_string(channel), std::to_string(row)}));
          }
        }
    }
    return changed;
  };
  walk(0);
  if (song.loopTo >= 0)
    for (int pass = 0; pass < 4 && walk(song.loopTo); pass++) {
    }
  // sound effects; in a song of samples, those on the TIA's channel 1 become tables of
  // AUDC1, AUDF1 and AUDV1 frame by frame, played as the editor plays them (channel $FF)
  std::vector<std::pair<std::string, std::pair<int, int>>> effects;  // symbol, sound (or table) and channel
  std::vector<std::vector<TiaFrame>> tiaTables;
  std::set<std::string> symbols;
  for (auto& effect : song.effects) {
    std::string symbol = effectSymbol(effect.name);
    if (!builder.valid(effect.instrument)) {
      builder.problem(F("Sound effect {0}: its instrument does not exist", {effect.name}));
      continue;
    }
    if (!symbols.insert(symbol).second) {
      builder.problem(F("Sound effect {0}: another one has the same name", {effect.name}));
      continue;
    }
    if (song.samples() && effect.tia) {
      int index = (int)(&effect - song.effects.data());
      bool endless = false;
      std::vector<TiaFrame> frames = tiaEffectFrames(song, audioClock, index, kMaxTiaEffectFrames, &endless);
      if (endless) builder.problem(F("Sound effect {0} on the TIA's channel 1 lasts more than {1} frames: its volume envelope must end on 0", {effect.name, std::to_string(kMaxTiaEffectFrames)}));
      effects.push_back({symbol, {(int)tiaTables.size(), 0xFF}});
      tiaTables.push_back(frames);
      continue;
    }
    // The player lets an effect go only when its volume reaches 0: nothing plays its note off
    const Envelope* volume = song.instruments[effect.instrument].volumes.empty() ? nullptr : &song.instruments[effect.instrument].volumes[0];
    if (!song.samples() && (!volume || volume->values.empty() || volume->loop >= 0 || volume->values.back() != 0))
      builder.problem(F("Sound effect {0} never ends and keeps its channel: its instrument's volume envelope must end on 0, with no loop", {effect.name}));
    effects.push_back({symbol, {builder.sound(effect.instrument, effect.note, 0, 0), std::clamp(effect.channel, 0, channels - 1)}});
  }
  if (builder.sounds.entries.size() > 250) builder.problem(F("{0} different sounds: the player takes 250", {std::to_string(builder.sounds.entries.size())}));
  if (builder.releases.entries.size() > 64) builder.problem(F("{0} different releases: the player takes 64", {std::to_string(builder.releases.entries.size())}));
  if (builder.envelopes.entries.size() > 256) builder.problem(F("{0} different volume envelopes: the player takes 256", {std::to_string(builder.envelopes.entries.size())}));
  if (song.order.size() > 254) builder.problem(T("The order list is longer than 254 entries."));
  result.problems = builder.problems;
  if (builder.sounds.entries.size() > 250 || builder.releases.entries.size() > 64 || builder.envelopes.entries.size() > 256 || song.order.size() > 254) return result;
  const bool cdfj = options.target == ExportTarget::Cdfj;
  if ((options.target == ExportTarget::C || dpc || cdfj) != song.samples()) {
    result.problems.push_back(song.samples() ? T("A song played as samples is exported as C (ELF or CDFJ) or for DPC+.")
                                             : T("C, CDFJ and DPC+ export are for songs played as samples; a TIA song is exported for DASM or batari Basic."));
    return result;
  }
  if (cdfj && channels > 3) {
    result.problems.push_back(T("CDFJ has 3 voices: the song has more."));
    return result;
  }
  if (dpc) {  // what DPC+ cannot hold stops the export
    int most = dpcMaxLevel(channels), waves = (int)builder.waves.entries.size();
    size_t before = result.problems.size();
    if (channels > 3) result.problems.push_back(T("DPC+ has 3 voices: the song has more."));
    if ((int)builder.frequencies.entries.size() > 255)
      result.problems.push_back(F("{0} different pitches: DPC+'s frequency table takes 255", {std::to_string(builder.frequencies.entries.size())}));
    if (waves * most + 1 > 128 || waves * (most + 1) > 256)
      result.problems.push_back(F("{0} waveforms at {1} volumes do not fit in DPC+'s 128 waveform slots", {std::to_string(waves), std::to_string(most)}));
    if (result.problems.size() > before) return result;
  }
  if (options.target == ExportTarget::C || cdfj) return writeC(song, builder, events, effects, tiaTables, options, result);

  // the text
  std::ostringstream out;
  out << "; " << song.name << " - music for the Atari 2600, made with VCSIDE's music editor.\n";
  out << "; Generated from the .vcsmusic file: change that one, not this.\n";
  out << "; Tuned for " << options.tv << ".\n";
  if (options.target == ExportTarget::BatariBasic) {
    out << ";\n; batari Basic: the player uses the variables " << options.ramStart << " and the 18 after it ("
        << options.ramStart << " to z if it is h): do not use them elsewhere. Call it with\n";
    out << ";     asm\n;     jsr MusicInit      (once)\n;     end\n";
    out << "; and, once per frame (before drawscreen):\n;     asm\n;     jsr MusicFrame\n;     end\n";
    out << "; and put 'inline <this file>' where the program never runs into it (after a goto).\n";
    out << "\nMUSIC_RAM = " << options.ramStart << "\n";
  } else if (dpc) {
    int ramBytes = 5 + 7 * channels + (tiaTables.empty() ? 0 : 3);
    out << ";\n; DPC+: include this file three times, with MUSIC_PART set before each:\n";
    out << ";   MUSIC_PART SET 1   in a bank of 6507 code: the player and the song (after DPCplus.h)\n";
    out << ";   MUSIC_PART SET 2   in the display data: the waveforms (" << (1 + (int)builder.waves.entries.size() * dpcMaxLevel(channels)) * 32 << " bytes)\n";
    out << ";   MUSIC_PART SET 3   at the start of the frequency table, instead of DPC_frequencies.h\n";
    out << "; Define MUSIC_RAM (" << ramBytes << " bytes of zero page) first. Call MusicInit once and MusicFrame once\n";
    out << "; per frame; with AUDC0 = 0, the kernel copies the voices' sum to AUDV0 on every line:\n";
    out << ";     lda AMPLITUDE\n;     sta AUDV0\n";
    out << "; MusicFrame does the same a few times while it works (it takes several lines).\n";
    out << "; Each voice's volume is one of " << dpcMaxLevel(channels) + 1 << " levels (DPC+ adds the voices and AUDV0 takes 4 bits),\n";
    out << "; and noise repeats every 32 steps of its register.\n";
    out << "\n  IFNCONST MUSIC_PART\n    ERR ; set MUSIC_PART to 1, 2 or 3 before including this file\n  ENDIF\n";
    out << "\n  IF MUSIC_PART = 1\n";
    out << "\n  IFNCONST MUSIC_RAM\n    ERR ; MUSIC_RAM is not defined: " << ramBytes << " bytes of zero page for the music player\n  ENDIF\n";
  } else {
    out << ";\n; Define MUSIC_RAM (19 bytes of zero page) before including this file.\n";
    out << "\n  IFNCONST MUSIC_RAM\n    ERR ; MUSIC_RAM is not defined: 19 bytes of zero page for the music player\n  ENDIF\n";
  }
  int dataBytes = 0;
  std::vector<int> orderRows;
  for (int patternIndex : song.order) orderRows.push_back(patternIndex >= 0 && patternIndex < (int)song.patterns.size() ? song.patterns[patternIndex].rows : 1);
  out << "\nMUSIC_SPEED = " << std::clamp(song.speed, 1, 255) << "\n";
  out << "MUSIC_SWING = " << std::clamp(song.swing, 0, 255) << "\n";
  out << "MUSIC_PATTERN_SPEEDS = " << (song.patternSpeeds() ? 1 : 0) << "  ; patterns with speeds of their own\n";
  out << "MUSIC_ORDER_LENGTH = " << song.order.size() << "\n";
  out << "MUSIC_LOOP_TO = " << (song.loopTo >= 0 ? std::to_string(song.loopTo) : "MUSIC_ORDER_LENGTH  ; stops at the end") << "\n";
  out << "MUSIC_CHANNELS = " << channels << "\n";
  out << "MUSIC_DPC = " << (dpc ? 1 : 0) << "\n";
  out << "MUSIC_TIA_EFFECTS = " << tiaTables.size() << "  ; sound effects on the TIA's channel 1\n";
  for (size_t index = 0; index < effects.size(); index++) out << effects[index].first << " = " << index << "\n";
  out << kPlayer;
  out << "\n; ---- the song ----\n";
  // order list
  out << "\nMusicOrderLo\n";
  std::string lows, highs;
  for (size_t entry = 0; entry < song.order.size(); entry++) {
    lows += listSeparator(entry) + "<MusicPattern" + std::to_string(song.order[entry]);
    highs += listSeparator(entry) + ">MusicPattern" + std::to_string(song.order[entry]);
  }
  out << lows << "\nMusicOrderHi\n" << highs << "\nMusicOrderRows\n" << byteList(orderRows);
  dataBytes += (int)song.order.size() * 3;
  if (song.patternSpeeds()) {
    std::vector<int> evenFrames, oddFrames;
    for (int patternIndex : song.order) {
      bool valid = patternIndex >= 0 && patternIndex < (int)song.patterns.size();
      evenFrames.push_back(std::clamp(valid ? song.rowFrames(song.patterns[patternIndex], 0) : song.speed, 1, 255));
      oddFrames.push_back(std::clamp(valid ? song.rowFrames(song.patterns[patternIndex], 1) : song.rowFrames(1), 1, 255));
    }
    out << "\nMusicOrderSpeed\n" << byteList(evenFrames) << "\nMusicOrderSwing\n" << byteList(oddFrames);
    dataBytes += (int)song.order.size() * 2;
  }
  // patterns (each one used by the order list, once)
  std::set<int> written;
  for (int patternIndex : song.order) {
    if (patternIndex < 0 || patternIndex >= (int)song.patterns.size() || !written.insert(patternIndex).second) continue;
    const Pattern& pattern = song.patterns[patternIndex];
    std::vector<int> bytes;
    for (int row = 0; row < pattern.rows; row++)
      for (int channel = 0; channel < channels; channel++) {
        Event event = events[std::make_tuple(patternIndex, channel, row)];
        bytes.push_back(event.code);
        if (event.argument >= 0) bytes.push_back(event.argument);
      }
    out << "MusicPattern" << patternIndex << (pattern.name.empty() ? "" : "  ; " + pattern.name) << "\n" << byteList(bytes);
    dataBytes += (int)bytes.size();
  }
  // sounds
  std::vector<int> soundEnvelope, soundRelease;
  std::string pitchLows, pitchHighs;
  for (size_t index = 0; index < builder.sounds.entries.size(); index++) {
    auto& [envelopeIndex, pitchIndex, releaseIndex] = builder.sounds.entries[index];
    soundEnvelope.push_back(envelopeIndex), soundRelease.push_back(releaseIndex);
    pitchLows += listSeparator(index) + "<MusicPitchData" + std::to_string(pitchIndex);
    pitchHighs += listSeparator(index) + ">MusicPitchData" + std::to_string(pitchIndex);
  }
  if (builder.sounds.entries.empty()) pitchLows = pitchHighs = "    .byte 0";
  out << "\nMusicSoundEnvelope\n" << (soundEnvelope.empty() ? "    .byte 0\n" : byteList(soundEnvelope));
  out << "MusicSoundRelease\n" << (soundRelease.empty() ? "    .byte 0\n" : byteList(soundRelease));
  out << "MusicSoundPitchLo\n" << pitchLows << "\nMusicSoundPitchHi\n" << pitchHighs << "\n";
  dataBytes += (int)builder.sounds.entries.size() * 4;
  // effects
  std::vector<int> effectSounds, effectChannels;
  for (auto& effect : effects) effectSounds.push_back(effect.second.first), effectChannels.push_back(effect.second.second);
  out << "MusicEffectSound\n" << (effects.empty() ? "    .byte 0\n" : byteList(effectSounds));
  out << "MusicEffectChannel\n" << (effects.empty() ? "    .byte 0\n" : byteList(effectChannels));
  dataBytes += (int)effects.size() * 2;
  // sound effects on the TIA's channel 1: frames + 1, then AUDC1, AUDF1 and AUDV1 for each frame
  if (!tiaTables.empty()) {
    std::string lows = "MusicTiaLo\n", highs = "MusicTiaHi\n";
    for (size_t index = 0; index < tiaTables.size(); index++) {
      lows += listSeparator(index) + "<MusicTiaData" + std::to_string(index);
      highs += listSeparator(index) + ">MusicTiaData" + std::to_string(index);
    }
    out << lows << "\n" << highs << "\n";
    for (size_t index = 0; index < tiaTables.size(); index++) {
      std::vector<int> bytes = {(int)tiaTables[index].size() + 1};
      for (auto& frame : tiaTables[index]) bytes.insert(bytes.end(), {frame.control, frame.frequency, frame.volume});
      out << "MusicTiaData" << index << "\n" << byteList(bytes);
      dataBytes += 2 + (int)bytes.size();
    }
  }
  // envelopes: length, loop ($FF none), values; releases: length, $FF, values
  auto pointerTables = [&](const char* name, const char* dataName, size_t count) {
    std::string low = std::string(name) + "Lo\n", high = std::string(name) + "Hi\n";
    for (size_t index = 0; index < count; index++) {
      low += listSeparator(index) + "<" + dataName + std::to_string(index);
      high += listSeparator(index) + ">" + dataName + std::to_string(index);
    }
    if (!count) low += "    .byte 0", high += "    .byte 0";
    out << low << "\n" << high << "\n";
    dataBytes += (int)count * 2;
  };
  pointerTables("MusicEnvelope", "MusicEnvelopeData", builder.envelopes.entries.size());
  for (size_t index = 0; index < builder.envelopes.entries.size(); index++) {
    auto& [values, loop] = builder.envelopes.entries[index];
    std::vector<int> bytes = {(int)values.size(), loop >= 0 ? loop : 0xff};
    bytes.insert(bytes.end(), values.begin(), values.end());
    out << "MusicEnvelopeData" << index << "\n" << byteList(bytes);
    dataBytes += (int)bytes.size();
  }
  pointerTables("MusicRelease", "MusicReleaseData", builder.releases.entries.size());
  for (size_t index = 0; index < builder.releases.entries.size(); index++) {
    auto& values = builder.releases.entries[index];
    std::vector<int> bytes = {(int)values.size(), 0xff};
    bytes.insert(bytes.end(), values.begin(), values.end());
    out << "MusicReleaseData" << index << "\n" << byteList(bytes);
    dataBytes += (int)bytes.size();
  }
  for (size_t index = 0; index < builder.pitches.entries.size(); index++) {
    out << "MusicPitchData" << index << "\n" << byteList(builder.pitches.entries[index]);
    dataBytes += (int)builder.pitches.entries[index].size();
  }
  if (dpc) {
    // a voice's volume to one of the wave's copies, and each wave's copies as DPC+ slots
    int most = dpcMaxLevel(channels);
    std::vector<int> levels;
    for (int volume = 0; volume < 16; volume++) levels.push_back(dpcLevel(volume, channels, song.gain));
    out << "MusicDpcLevel\n" << byteList(levels);
    out << "MUSIC_WAVE_OFF = [MusicWaveOff & $1FFF] / 32 & $7F\n";
    out << "MusicWaveSlots\n";
    for (size_t wave = 0; wave < builder.waves.entries.size(); wave++) {
      out << "    .byte MUSIC_WAVE_OFF\n";
      for (int level = 1; level <= most; level++) out << "    .byte [MusicWave" << wave << "_" << level << " & $1FFF] / 32 & $7F\n";
    }
    dataBytes += 16 + (int)builder.waves.entries.size() * (most + 1);
    out << "\n  ENDIF  ; MUSIC_PART = 1\n";
    // the waveforms, at each level, in the display data
    out << "\n  IF MUSIC_PART = 2\n    ALIGN 32\nMusicWaveOff\n";
    out << byteList(std::vector<int>(32, 0));
    for (size_t wave = 0; wave < builder.waves.entries.size(); wave++)
      for (int level = 1; level <= most; level++) {
        std::vector<int> samples;
        for (int sample : builder.waves.entries[wave]) samples.push_back((int)std::lround(sample * level / 15.0));
        out << "MusicWave" << wave << "_" << level << "\n" << byteList(samples);
      }
    out << "\n  ENDIF  ; MUSIC_PART = 2\n";
    // the frequency table: 2^32 x Hz / 20000, entry 0 silent
    out << "\n  IF MUSIC_PART = 3\nMusicFrequencies\n    .long 0\n";
    for (long long frequency : builder.frequencies.entries) out << "    .long " << frequency << "\n";
    out << "  ENDIF  ; MUSIC_PART = 3\n";
  }
  result.ok = true;
  result.text = out.str();
  result.dataBytes = dataBytes;
  result.sounds = (int)builder.sounds.entries.size();
  result.envelopes = (int)builder.envelopes.entries.size();
  result.releases = (int)builder.releases.entries.size();
  result.pitches = (int)builder.pitches.entries.size();
  return result;
}

}  // namespace vcs::music
