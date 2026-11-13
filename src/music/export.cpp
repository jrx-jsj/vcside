// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "export.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "../i18n.h"
#include "../editor/indentation.h"
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
;   jsr MusicInit       once, at power on: song 0 from its start
;   lda #MUSIC_SONG_x
;   jsr MusicPlay       a song from its start (sound effects that play go on)
;   jsr MusicStop       the song lets its notes go and stops
;   jsr MusicFrame      once per frame (in the vertical blank or the overscan)
;   lda #MUSIC_EFFECT_x
;   jsr MusicEffect     a sound effect: the song's notes on its channel wait
;                       until it has ended
;
; The order list holds every song one after another, each one closed by an
; entry of 0 rows whose low byte is the entry it loops to (itself: it stops).
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
    bpl .clear              ; and on to MusicPlay with song 0

MusicPlay SUBROUTINE        ; A = the song
    tay
    lda #0
    sta MusicRow
    sta MusicWait
    ; the last song's voices go quiet; a sound effect keeps its own
    bit MusicPitchIndex0
    bmi .kept0
    sta MusicState0
.kept0
  IF MUSIC_CHANNELS > 1
    bit MusicPitchIndex1
    bmi .kept1
    sta MusicState1
.kept1
  ENDIF
  IF MUSIC_CHANNELS > 2
    bit MusicPitchIndex2
    bmi .kept2
    sta MusicState2
.kept2
  ENDIF
    ldx MusicSongStart,y
    jmp MusicEnter          ; its first entry

MusicStop SUBROUTINE        ; on the next frame the notes are let go, as at a song's end
    ldx #MUSIC_ORDER_LENGTH-1
    stx MusicOrder
    lda #0
    sta MusicWait
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
    ldx MusicOrder
    lda MusicOrderRows,x
    bne .row
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
    lda MusicOrderRows,x
    bne .entry
    lda MusicOrderLo,x      ; the song's end: the entry it loops to, or itself
    tax
.entry
MusicEnter                  ; X: the order entry the song goes on from
    stx MusicOrder
    lda MusicOrderRows,x
    beq .done               ; it stops: MusicFrame lets the notes go
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
 *   music_init();         once, at power on: song 0 from its start
 *   music_play(MUSIC_SONG_x);   a song from its start (sound effects that play go on)
 *   music_stop();         the song lets its notes go and stops
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
 * The TIA's channel 1 (MUSIC_TIA_EFFECTS, MUSIC_TIA_TRACK): music_frame leaves this
 * frame's AUDC1, AUDF1 and AUDV1 in music_tia[0], [1] and [2], for the game to write
 * once per frame: the song's TIA track, or a sound effect there, which takes the track
 * until it ends (AUDV1 is 0 when neither sounds); music_tia_left is 0 when no effect
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
/* always there, so a game that writes them keeps building with or without them */
uint8_t music_tia[3];               /* AUDC1, AUDF1 and AUDV1 for this frame */
uint8_t music_tia_left;             /* frames left of the effect + 1 (0: none) */
#if MUSIC_TIA_EFFECTS
static uint16_t music_tia_at;       /* its next frame, in music_tia_data */
#endif
#if MUSIC_TIA_TRACK
/* the TIA track: pitch is in music_tia_pitch_data; pitch_index bit 7: an effect has it */
static music_voice music_tia_track;
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

#if MUSIC_TIA_TRACK
static void music_tia_start(int sound)
{
    music_voice *voice = &music_tia_track;
    voice->pitch = music_tia_sound_pitch[sound];
    voice->state = (uint8_t)(music_tia_sound_release[sound] | 0x80);
    voice->volume = music_envelope_offset[music_tia_sound_envelope[sound]];
    voice->volume_index = 0;
    voice->pitch_index = 0;
}
#endif

static void music_note_off(music_voice *voice)
{
    if (!(voice->state & 0x80) || (voice->state & 0x40)) return;
    voice->state |= 0x40;
    voice->volume = music_release_offset[voice->state & 0x3F];
    voice->volume_index = 0;
}

/* The order list holds every song one after another, each one closed by an entry of
 * 0 rows whose offset is the entry it loops to (itself: it stops). */
void music_play(int song)
{
    music_order = music_song_start[song];
    music_row = music_wait = 0;
    music_pointer = music_order_offset[music_order];
    for (int i = 0; i < MUSIC_VOICES; i++)
        if (!(music_voices[i].pitch_index & 0x80)) music_voices[i].state = 0;   /* a sound effect keeps its voice */
#if MUSIC_TIA_TRACK
    if (!(music_tia_track.pitch_index & 0x80)) music_tia_track.state = 0;
#endif
}

/* On the next frame the notes are let go, as at a song's end. */
void music_stop(void)
{
    music_order = MUSIC_ORDER_LENGTH - 1;
    music_wait = 0;
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
    music_tia_left = 0;
    music_tia[0] = music_tia[1] = music_tia[2] = 0;
#if MUSIC_TIA_TRACK
    music_tia_track.state = 0;
    music_tia_track.pitch_index = 0;
#endif
    music_play(0);
}

void music_effect(int effect)
{
#if MUSIC_TIA_EFFECTS
    if (music_effect_voice[effect] == 0xFF) {           /* on the TIA's channel 1 */
        music_tia_at = music_tia_offset[music_effect_sound[effect]];
        music_tia_left = music_tia_data[music_tia_at++];
#if MUSIC_TIA_TRACK
        music_tia_track.state = 0;                      /* the track waits until it ends */
        music_tia_track.pitch_index = 0x80;
#endif
        return;
    }
#endif
    music_voice *voice = &music_voices[music_effect_voice[effect]];
    music_start(voice, music_effect_sound[effect]);
    voice->pitch_index = 0x80;
}

/* A row's event for a voice (tia: the TIA track), from music_patterns[at]; returns
 * where the next one is. */
static int music_event(music_voice *voice, int at, int tia)
{
    int code = music_patterns[at++], argument = 0;
    if (code >= 2 && code <= 4) argument = music_patterns[at++];
    if (voice->pitch_index & 0x80) return at;          /* a sound effect has the voice */
    if (code >= 6) {
#if MUSIC_TIA_TRACK
        if (tia) {
            music_tia_start(code - 6);
            return at;
        }
#endif
        (void)tia;
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
    for (int i = 0; i < MUSIC_VOICES; i++) at = music_event(&music_voices[i], at, 0);
#if MUSIC_TIA_TRACK
    at = music_event(&music_tia_track, at, 1);
#endif
    music_pointer = (uint16_t)at;
#if MUSIC_PATTERN_SPEEDS
    music_wait = (music_row & 1) ? music_order_swing[music_order] : music_order_speed[music_order];
#else
    music_wait = (MUSIC_SWING > 0 && (music_row & 1)) ? MUSIC_SWING : MUSIC_SPEED;
#endif
    if (++music_row >= music_order_rows[music_order]) {
        music_row = 0;
        music_order++;
        if (!music_order_rows[music_order]) music_order = (uint8_t)music_order_offset[music_order];  /* the song's end: its loop, or itself */
        if (music_order_rows[music_order]) music_pointer = music_order_offset[music_order];
    }
}

/* This frame's volume of a voice, from its envelope or its release; -1: it is silent
 * (it was not sounding, or it stops now), and its channel is the song's again. */
static int music_volume_step(music_voice *voice)
{
    if (!(voice->state & 0x80)) {
        voice->pitch_index = 0;
        return -1;
    }
    const uint8_t *volume = music_volume_data + voice->volume;
    int index = voice->volume_index;
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
    voice->volume_index = (uint8_t)(index + 1);
    return volume[2 + index];
stop:
    voice->state = 0;
    voice->pitch_index = 0;
    return -1;
}

/* This frame's volume and phase step for a voice. */
static void music_voice_frame(music_voice *voice)
{
    int value = music_volume_step(voice);
    if (value < 0) {
        voice->level = 0;
        return;
    }
    const uint32_t *pitch = music_pitch_data + voice->pitch;
    int length = (int)(pitch[0] & 0xFF), step = voice->pitch_index & 0x7F;
    if (step != length - 1) voice->pitch_index++;
    voice->step = pitch[1 + step];
    voice->wave = (uint8_t)(pitch[0] >> 8);
    voice->level = (uint8_t)value;
}

#if MUSIC_TIA_TRACK
/* The TIA track's AUDC1, AUDF1 and AUDV1 for this frame, as a channel of a TIA song. */
static void music_tia_track_frame(void)
{
    music_voice *voice = &music_tia_track;
    int value = music_volume_step(voice);
    if (value < 0) {
        music_tia[2] = 0;
        return;
    }
    const uint8_t *pitch = music_tia_pitch_data + voice->pitch;
    int length = pitch[0] & 0x7F, step = voice->pitch_index & 0x7F;
    if (step != length - 1) voice->pitch_index++;
    music_tia[0] = pitch[1 + 2 * step];
    music_tia[1] = pitch[2 + 2 * step];
    music_tia[2] = (uint8_t)((pitch[0] & 0x80) ? 0 : value);   /* no allowed AUDC reaches the note */
}
#endif

void music_frame(int lines)
{
    if (music_order != 0xFF) {
        if (music_wait == 0) {
            if (!music_order_rows[music_order]) {       /* the song is over: what sounds is released */
                for (int i = 0; i < MUSIC_VOICES; i++)
                    if (!(music_voices[i].pitch_index & 0x80)) music_note_off(&music_voices[i]);
#if MUSIC_TIA_TRACK
                if (!(music_tia_track.pitch_index & 0x80)) music_note_off(&music_tia_track);
#endif
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
    if (music_tia_left) {
        if (--music_tia_left) {                         /* the effect's frame on the TIA's channel 1 */
            music_tia[0] = music_tia_data[music_tia_at];
            music_tia[1] = music_tia_data[music_tia_at + 1];
            music_tia[2] = music_tia_data[music_tia_at + 2];
            music_tia_at += 3;
        } else {
            music_tia[2] = 0;                           /* it is over: silent, and the track is the song's again */
#if MUSIC_TIA_TRACK
            music_tia_track.pitch_index = 0;
#endif
        }
    } else {
#if MUSIC_TIA_TRACK
        music_tia_track_frame();
#else
        music_tia[2] = 0;
#endif
    }
#elif MUSIC_TIA_TRACK
    music_tia_track_frame();
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

// The C player for TIA songs (an ELF game, or batari Basic's PXE kernel): the same rules as
// the 6502 player, a register set per channel and frame. CC0 like the others.
static const char* kCTiaPlayer = R"PLAYER(
/* ---------------------------------------------------------------------------
 * VCSIDE music player in C for TIA songs: each channel's AUDC, AUDF and AUDV, once
 * per frame. Dedicated to the public domain under CC0 1.0: use it in any game, with
 * no licence or credit needed.
 *
 *   music_init();         once, at power on: song 0 from its start
 *   music_play(MUSIC_SONG_x);   a song from its start (sound effects that play go on)
 *   music_stop();         the song lets its notes go and stops
 *   music_effect(n);      a sound effect (MUSIC_EFFECT_x) on its channel: the song's
 *                         notes there wait until it has ended
 *   music_frame();        once per frame; then music_audio[c][0], [1] and [2] are
 *                         AUDCc, AUDFc and AUDVc for channel c, for the game to write
 * ------------------------------------------------------------------------- */

typedef struct {
    uint16_t volume;        /* the envelope or the release, in music_volume_data */
    uint8_t volume_index;
    uint8_t pitch_index;    /* bit 7: a sound effect has the channel */
    uint16_t pitch;         /* the sound's AUDC and AUDF steps, in music_pitch_data */
    uint8_t state;          /* bit 7 sounding, bit 6 released, bits 0-5 the release */
} music_voice;

static music_voice music_voices[MUSIC_CHANNELS];
static uint8_t music_order, music_row, music_wait;   /* music_order 0xFF: stopped */
static uint16_t music_pointer;                       /* the next row, in music_patterns */
uint8_t music_audio[MUSIC_CHANNELS][3];              /* AUDC, AUDF and AUDV of each channel */

static void music_start(music_voice *voice, int sound)
{
    voice->pitch = music_sound_pitch[sound];
    voice->state = (uint8_t)(music_sound_release[sound] | 0x80);
    voice->volume = music_envelope_offset[music_sound_envelope[sound]];
    voice->volume_index = 0;
    voice->pitch_index = 0;
}

static void music_note_off(music_voice *voice)
{
    if (!(voice->state & 0x80) || (voice->state & 0x40)) return;
    voice->state |= 0x40;
    voice->volume = music_release_offset[voice->state & 0x3F];
    voice->volume_index = 0;
}

/* The order list holds every song one after another, each one closed by an entry of
 * 0 rows whose offset is the entry it loops to (itself: it stops). */
void music_play(int song)
{
    music_order = music_song_start[song];
    music_row = music_wait = 0;
    music_pointer = music_order_offset[music_order];
    for (int i = 0; i < MUSIC_CHANNELS; i++)
        if (!(music_voices[i].pitch_index & 0x80)) music_voices[i].state = 0;   /* a sound effect keeps its channel */
}

/* On the next frame the notes are let go, as at a song's end. */
void music_stop(void)
{
    music_order = MUSIC_ORDER_LENGTH - 1;
    music_wait = 0;
}

void music_init(void)
{
    for (int i = 0; i < MUSIC_CHANNELS; i++) {
        music_voices[i].state = 0;
        music_voices[i].pitch_index = 0;
        music_audio[i][0] = music_audio[i][1] = music_audio[i][2] = 0;
    }
    music_play(0);
}

void music_effect(int effect)
{
    music_voice *voice = &music_voices[music_effect_channel[effect]];
    music_start(voice, music_effect_sound[effect]);
    voice->pitch_index = 0x80;
}

/* A row's event for a channel, from music_patterns[at]; returns where the next one is. */
static int music_event(music_voice *voice, int at)
{
    int code = music_patterns[at++], argument = 0;
    if (code >= 2 && code <= 4) argument = music_patterns[at++];
    if (voice->pitch_index & 0x80) return at;          /* a sound effect has the channel */
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
    for (int i = 0; i < MUSIC_CHANNELS; i++) at = music_event(&music_voices[i], at);
    music_pointer = (uint16_t)at;
#if MUSIC_PATTERN_SPEEDS
    music_wait = (music_row & 1) ? music_order_swing[music_order] : music_order_speed[music_order];
#else
    music_wait = (MUSIC_SWING > 0 && (music_row & 1)) ? MUSIC_SWING : MUSIC_SPEED;
#endif
    if (++music_row >= music_order_rows[music_order]) {
        music_row = 0;
        music_order++;
        if (!music_order_rows[music_order]) music_order = (uint8_t)music_order_offset[music_order];  /* the song's end: its loop, or itself */
        if (music_order_rows[music_order]) music_pointer = music_order_offset[music_order];
    }
}

/* This frame's volume of a channel, from its envelope or its release; -1: it is silent
 * (it was not sounding, or it stops now), and the channel is the song's again. */
static int music_volume_step(music_voice *voice)
{
    if (!(voice->state & 0x80)) {
        voice->pitch_index = 0;
        return -1;
    }
    const uint8_t *volume = music_volume_data + voice->volume;
    int index = voice->volume_index;
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
    voice->volume_index = (uint8_t)(index + 1);
    return volume[2 + index];
stop:
    voice->state = 0;
    voice->pitch_index = 0;
    return -1;
}

/* A channel's AUDC, AUDF and AUDV for this frame. */
static void music_channel_frame(int channel)
{
    music_voice *voice = &music_voices[channel];
    uint8_t *audio = music_audio[channel];
    int value = music_volume_step(voice);
    if (value < 0) {
        audio[2] = 0;
        return;
    }
    const uint8_t *pitch = music_pitch_data + voice->pitch;
    int length = pitch[0] & 0x7F, step = voice->pitch_index & 0x7F;
    if (step != length - 1) voice->pitch_index++;
    audio[0] = pitch[1 + 2 * step];
    audio[1] = pitch[2 + 2 * step];
    audio[2] = (uint8_t)((pitch[0] & 0x80) ? 0 : value);   /* no allowed AUDC reaches the note */
}

void music_frame(void)
{
    if (music_order != 0xFF) {
        if (music_wait == 0) {
            if (!music_order_rows[music_order]) {       /* the song is over: what sounds is released */
                for (int i = 0; i < MUSIC_CHANNELS; i++)
                    if (!(music_voices[i].pitch_index & 0x80)) music_note_off(&music_voices[i]);
                music_order = 0xFF;
                goto channels;
            }
            music_play_row();
        }
        music_wait--;
    }
channels:
    for (int i = 0; i < MUSIC_CHANNELS; i++) music_channel_frame(i);
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

const char* exportFileName(ExportTarget target) {
  return target == ExportTarget::C || target == ExportTarget::Cdfj ? "music.h" : "music.asm";
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
  Builder(const Song& songToExport, double audioClock, bool forDpc = false) : song(&songToExport), clock(audioClock), dpc(forDpc) {}
  const Song* song;  // the song being walked: the pools are shared by every song of the export
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
  // samples: the TIA track's, played on the TIA (pitches as in a TIA song)
  Pool<std::vector<int>> tiaPitches;
  Pool<std::tuple<int, int, int>> tiaSounds;

  void problem(const std::string& text) {
    if (said.insert(text).second) problems.push_back(text);
  }
  bool valid(int instrument) const { return instrument >= 0 && instrument < (int)song->instruments.size(); }

  int envelope(int instrument, int variantIndex) {
    const Instrument& source = song->instruments[instrument];
    const Envelope& chosen = source.volumes[variantIndex >= 0 && variantIndex < (int)source.volumes.size() ? variantIndex : 0];
    std::vector<int> values(chosen.values.begin(), chosen.values.begin() + std::min<size_t>(chosen.values.size(), 250));
    int loop = chosen.loop >= 0 && chosen.loop < (int)values.size() ? chosen.loop : -1;
    return envelopes.add({values, loop});
  }
  int release(int instrument, int variantIndex) {
    const Instrument& source = song->instruments[instrument];
    const std::vector<int>& chosen = source.releases[variantIndex >= 0 && variantIndex < (int)source.releases.size() ? variantIndex : 0];
    return releases.add(std::vector<int>(chosen.begin(), chosen.begin() + std::min<size_t>(chosen.size(), 250)));
  }
  // onTia: the TIA track of a song of samples (a TIA pitch, in tiaPitches)
  int pitch(int instrument, int note, bool onTia = false) {
    const Instrument& source = song->instruments[instrument];
    std::vector<int> bytes;
    bool muted = false;
    std::vector<std::pair<int, int>> steps;
    if (song->samples() && !onTia) {
      std::vector<uint32_t> phaseSteps = song->phaseSteps(note, source, clock);
      if (phaseSteps.size() > 120) {
        problem(F("{0}: only the first 120 frames of its pitch are exported", {source.name}));
        phaseSteps.resize(120);
      }
      if (dpc) {
        auto levels = dpcWave(source);
        int wave = waves.add(std::vector<int>(levels.begin(), levels.end()));
        bytes.push_back((int)phaseSteps.size());
        for (uint32_t step : phaseSteps)
          bytes.push_back((wave * (dpcMaxLevel(song->channelCount()) + 1)) & 0xFF), bytes.push_back((1 + frequencies.add(dpcFrequency(step, clock))) & 0xFF);
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
      std::vector<PitchStep> sequence = song->pitchSequence(note, source, clock, onTia);
      muted = !sequence[0].playable;
      if (muted) problem(F("{0} with {1}: no allowed AUDC reaches it, so it is silent", {noteName(note + song->tuning.transpose), source.name}));
      for (auto& step : sequence) steps.push_back({step.control, step.frequency});
    }
    if (steps.size() > 120) {
      problem(F("{0}: only the first 120 frames of its pitch are exported", {source.name}));
      steps.resize(120);
    }
    bytes.push_back((int)steps.size() | (muted ? 0x80 : 0));
    for (auto& [control, frequency] : steps) bytes.push_back(control & 15), bytes.push_back(frequency & 31);
    return onTia && song->samples() ? tiaPitches.add(bytes) : pitches.add(bytes);
  }
  int sound(int instrument, int note, int volumeVariant, int releaseVariant, bool onTia = false) {
    std::tuple<int, int, int> made{envelope(instrument, volumeVariant), pitch(instrument, note, onTia), release(instrument, releaseVariant)};
    return onTia && song->samples() ? tiaSounds.add(made) : sounds.add(made);
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

namespace {

// One entry of the order list every song shares: a pattern of a song, or the entry of 0
// rows that closes a song (its low byte or offset: the entry the song loops to).
struct OrderEntry {
  int song = 0, pattern = -1, rows = 0, even = 1, odd = 1;
  int loopTo = -1;  // the closing entry: where the song goes on (its own index: it stops)
  bool closing() const { return rows == 0; }
};

// What every target writes: the songs walked into events, their order list and effects.
struct Combined {
  std::vector<NamedSong> songs;
  std::vector<OrderEntry> order;
  std::vector<int> songStart;
  std::map<std::tuple<int, int, int, int>, Event> events;            // song, pattern, channel, row
  std::vector<std::pair<std::string, std::pair<int, int>>> effects;  // symbol, sound (or table) and channel
  std::vector<std::vector<TiaFrame>> tiaTables;
  std::vector<std::string> songSymbols;
  int channels = 2, gain = 16, speed = 6, swing = 0;
  int tracks = 2;  // the channels, and the TIA track if a song has one (its events come after the voices')
  bool patternSpeeds = false;
  // the patterns each song plays, once each, in the order they are first played
  std::vector<std::pair<int, int>> patterns() const {
    std::vector<std::pair<int, int>> list;
    std::set<std::pair<int, int>> seen;
    for (auto& entry : order)
      if (!entry.closing() && entry.pattern >= 0 && seen.insert({entry.song, entry.pattern}).second) list.push_back({entry.song, entry.pattern});
    return list;
  }
  std::vector<int> patternBytes(int song, int patternIndex) const {
    std::vector<int> bytes;
    const Pattern& pattern = songs[song].song->patterns[patternIndex];
    for (int row = 0; row < pattern.rows; row++)
      for (int channel = 0; channel < tracks; channel++) {
        auto found = events.find(std::make_tuple(song, patternIndex, channel, row));
        Event event = found == events.end() ? Event{} : found->second;
        bytes.push_back(event.code);
        if (event.argument >= 0) bytes.push_back(event.argument);
      }
    return bytes;
  }
};

// The songs' own information at the top of the export.
std::string songList(const Combined& combined, const char* comment) {
  std::string text;
  for (size_t index = 0; index < combined.songs.size(); index++)
    text += std::string(comment) + "   " + combined.songSymbols[index] + " = " + std::to_string(index) + ": " + combined.songs[index].name + "\n";
  return text;
}

}  // namespace

// The songs as C data (no pointers: the tables stay in flash), then the C player.
static ExportResult writeC(const Combined& combined, Builder& builder, const ExportOptions& options, ExportResult& result) {
  std::ostringstream out;
  int dataBytes = 0;
  const int channels = combined.channels;
  const bool cdfj = options.target == ExportTarget::Cdfj;
  const auto& effects = combined.effects;
  const auto& tiaTables = combined.tiaTables;
  out << "/* Music for an ELF cartridge of the Atari 2600, made with VCSIDE's music editor: every\n";
  out << " * song of this folder, with one player. Generated from the .vcsmusic files: change those,\n";
  out << " * not this. Tuned for " << options.tv << ". Include it once, in the C file that calls the player.\n";
  out << " * The songs, for music_play:\n" << songList(combined, " *") << " */\n";
  out << "#ifndef MUSIC_H_INCLUDED\n#define MUSIC_H_INCLUDED\n#include <stdint.h>\n\n";
  out << "#define MUSIC_VOICES " << channels << "\n";
  out << "#define MUSIC_CDFJ " << (cdfj ? 1 : 0) << "  /* the cartridge plays the voices (see the player below) */\n";
  out << "#define MUSIC_SPEED " << std::clamp(combined.speed, 1, 255) << "\n";
  out << "#define MUSIC_SWING " << std::clamp(combined.swing, 0, 255) << "\n";
  out << "#define MUSIC_PATTERN_SPEEDS " << (combined.patternSpeeds ? 1 : 0) << "  /* patterns with speeds of their own */\n";
  out << "#define MUSIC_ORDER_LENGTH " << combined.order.size() << "  /* every song's entries and the one that closes it */\n";
  out << "#define MUSIC_SONGS " << combined.songs.size() << "\n";
  for (size_t index = 0; index < combined.songs.size(); index++) out << "#define " << combined.songSymbols[index] << " " << index << "\n";
  out << "#define MUSIC_MIX " << mixFactor(combined.gain, channels) << "u  /* the sum of the voices to quarters of a level, in 1/65536 */\n";
  out << "#define MUSIC_TIA_EFFECTS " << tiaTables.size() << "  /* sound effects on the TIA's channel 1 (music_tia) */\n";
  const bool tiaTrack = combined.tracks > channels;
  out << "#define MUSIC_TIA_TRACK " << (tiaTrack ? 1 : 0) << "  /* a track on the TIA's channel 1 (music_tia) */\n";
  for (size_t index = 0; index < effects.size(); index++) out << "#define " << effects[index].first << " " << index << "\n";
  // patterns, one after another, and where each order entry starts
  std::vector<long long> patternBytes, orderOffset, orderRows, songStart;
  std::map<std::pair<int, int>, int> patternAt;
  for (auto& [song, patternIndex] : combined.patterns()) {
    patternAt[{song, patternIndex}] = (int)patternBytes.size();
    for (int byte : combined.patternBytes(song, patternIndex)) patternBytes.push_back(byte);
  }
  for (auto& entry : combined.order) {
    bool valid = !entry.closing() && patternAt.count({entry.song, entry.pattern}) > 0;
    orderOffset.push_back(entry.closing() ? entry.loopTo : valid ? patternAt[{entry.song, entry.pattern}] : 0);
    orderRows.push_back(entry.rows);
  }
  for (int start : combined.songStart) songStart.push_back(start);
  if (patternBytes.size() > 65535) {
    result.problems.push_back(T("The songs are too long for the C player (more than 64 KB of patterns)."));
    return result;
  }
  if (patternBytes.empty()) patternBytes.push_back(0);
  out << "\nstatic const uint8_t music_patterns[] = {\n" << cList(patternBytes) << "};\n";
  out << "static const uint16_t music_order_offset[] = {\n" << cList(orderOffset) << "};\n";
  out << "static const uint8_t music_order_rows[] = {\n" << cList(orderRows) << "};\n";
  out << "static const uint8_t music_song_start[] = {\n" << cList(songStart) << "};\n";
  if (combined.patternSpeeds) {
    std::vector<long long> evenFrames, oddFrames;
    for (auto& entry : combined.order) evenFrames.push_back(entry.even), oddFrames.push_back(entry.odd);
    out << "static const uint8_t music_order_speed[] = {\n" << cList(evenFrames) << "};\n";
    out << "static const uint8_t music_order_swing[] = {\n" << cList(oddFrames) << "};\n";
    dataBytes += (int)combined.order.size() * 2;
  }
  dataBytes += (int)patternBytes.size() + (int)combined.order.size() * 3 + (int)songStart.size();
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
  // the TIA track's sounds: their pitch as in a TIA song (length, + 0x80 silent; AUDC and AUDF pairs)
  if (tiaTrack) {
    std::vector<long long> tiaPitchData, tiaPitchAt, tiaEnvelope, tiaRelease, tiaPitch;
    for (auto& entry : builder.tiaPitches.entries) {
      tiaPitchAt.push_back((long long)tiaPitchData.size());
      tiaPitchData.insert(tiaPitchData.end(), entry.begin(), entry.end());
    }
    for (auto& [envelopeIndex, pitchIndex, releaseIndex] : builder.tiaSounds.entries)
      tiaEnvelope.push_back(envelopeIndex), tiaRelease.push_back(releaseIndex), tiaPitch.push_back(tiaPitchAt[pitchIndex]);
    for (auto* list : {&tiaPitchData, &tiaEnvelope, &tiaRelease, &tiaPitch})
      if (list->empty()) list->push_back(0);
    out << "static const uint8_t music_tia_pitch_data[] = {\n" << cList(tiaPitchData) << "};\n";
    out << "static const uint8_t music_tia_sound_envelope[] = {\n" << cList(tiaEnvelope) << "};\n";
    out << "static const uint8_t music_tia_sound_release[] = {\n" << cList(tiaRelease) << "};\n";
    out << "static const uint16_t music_tia_sound_pitch[] = {\n" << cList(tiaPitch) << "};\n";
    dataBytes += (int)tiaPitchData.size() + (int)builder.tiaSounds.entries.size() * 4;
  }
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
    for (int volume = 0; volume < 16; volume++) levels.push_back(dpcLevel(volume, channels, combined.gain));
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
  result.text = indentWithTabs(out.str(), 4, false);  // tabs, as the templates (the " *" of comments stays)
  result.dataBytes = dataBytes;
  result.sounds = (int)builder.sounds.entries.size();
  result.envelopes = (int)builder.envelopes.entries.size();
  result.releases = (int)builder.releases.entries.size();
  result.pitches = (int)builder.pitches.entries.size();
  return result;
}

// C for TIA songs: the 6502 player's data, as C tables, and kCTiaPlayer.
static ExportResult writeCTia(const Combined& combined, Builder& builder, const ExportOptions& options, ExportResult& result) {
  std::ostringstream out;
  int dataBytes = 0;
  const int channels = combined.channels;
  const auto& effects = combined.effects;
  out << "/* Music for the Atari 2600 in C, made with VCSIDE's music editor: every song of this\n";
  out << " * folder, with one player, for an ELF game or batari Basic's PXE kernel. Generated from\n";
  out << " * the .vcsmusic files: change those, not this. Tuned for " << options.tv << ". Include it\n";
  out << " * once, in the C file that calls the player. The songs, for music_play:\n" << songList(combined, " *") << " */\n";
  out << "#ifndef MUSIC_H_INCLUDED\n#define MUSIC_H_INCLUDED\n#include <stdint.h>\n\n";
  out << "#define MUSIC_TIA 1  /* a TIA song: music_audio holds each channel's registers */\n";
  out << "#define MUSIC_CHANNELS " << channels << "\n";
  out << "#define MUSIC_SPEED " << std::clamp(combined.speed, 1, 255) << "\n";
  out << "#define MUSIC_SWING " << std::clamp(combined.swing, 0, 255) << "\n";
  out << "#define MUSIC_PATTERN_SPEEDS " << (combined.patternSpeeds ? 1 : 0) << "  /* patterns with speeds of their own */\n";
  out << "#define MUSIC_ORDER_LENGTH " << combined.order.size() << "  /* every song's entries and the one that closes it */\n";
  out << "#define MUSIC_SONGS " << combined.songs.size() << "\n";
  for (size_t index = 0; index < combined.songs.size(); index++) out << "#define " << combined.songSymbols[index] << " " << index << "\n";
  for (size_t index = 0; index < effects.size(); index++) out << "#define " << effects[index].first << " " << index << "\n";
  out << "#define MUSIC_EFFECTS " << effects.size() << "\n";
  std::vector<long long> patternBytes, orderOffset, orderRows, songStart;
  std::map<std::pair<int, int>, int> patternAt;
  for (auto& [song, patternIndex] : combined.patterns()) {
    patternAt[{song, patternIndex}] = (int)patternBytes.size();
    for (int byte : combined.patternBytes(song, patternIndex)) patternBytes.push_back(byte);
  }
  for (auto& entry : combined.order) {
    bool valid = !entry.closing() && patternAt.count({entry.song, entry.pattern}) > 0;
    orderOffset.push_back(entry.closing() ? entry.loopTo : valid ? patternAt[{entry.song, entry.pattern}] : 0);
    orderRows.push_back(entry.rows);
  }
  for (int start : combined.songStart) songStart.push_back(start);
  if (patternBytes.size() > 65535) {
    result.problems.push_back(T("The songs are too long for the C player (more than 64 KB of patterns)."));
    return result;
  }
  if (patternBytes.empty()) patternBytes.push_back(0);
  out << "\nstatic const uint8_t music_patterns[] = {\n" << cList(patternBytes) << "};\n";
  out << "static const uint16_t music_order_offset[] = {\n" << cList(orderOffset) << "};\n";
  out << "static const uint8_t music_order_rows[] = {\n" << cList(orderRows) << "};\n";
  out << "static const uint8_t music_song_start[] = {\n" << cList(songStart) << "};\n";
  if (combined.patternSpeeds) {
    std::vector<long long> evenFrames, oddFrames;
    for (auto& entry : combined.order) evenFrames.push_back(entry.even), oddFrames.push_back(entry.odd);
    out << "static const uint8_t music_order_speed[] = {\n" << cList(evenFrames) << "};\n";
    out << "static const uint8_t music_order_swing[] = {\n" << cList(oddFrames) << "};\n";
    dataBytes += (int)combined.order.size() * 2;
  }
  dataBytes += (int)patternBytes.size() + (int)combined.order.size() * 3 + (int)songStart.size();
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
  // pitches: the length (+ 0x80 silent), then AUDC and AUDF for each frame
  std::vector<long long> pitchData, pitchAt;
  for (auto& entry : builder.pitches.entries) {
    pitchAt.push_back((long long)pitchData.size());
    pitchData.insert(pitchData.end(), entry.begin(), entry.end());
  }
  if (pitchData.empty()) pitchData = {1, 0, 0}, pitchAt.push_back(0);
  out << "static const uint8_t music_pitch_data[] = {\n" << cList(pitchData) << "};\n";
  dataBytes += (int)pitchData.size();
  std::vector<long long> soundEnvelope, soundRelease, soundPitch, effectSound, effectChannel;
  for (auto& [envelopeIndex, pitchIndex, releaseIndex] : builder.sounds.entries)
    soundEnvelope.push_back(envelopeIndex), soundRelease.push_back(releaseIndex), soundPitch.push_back(pitchAt[pitchIndex]);
  for (auto& effect : effects) effectSound.push_back(effect.second.first), effectChannel.push_back(effect.second.second);
  for (auto* list : {&soundEnvelope, &soundRelease, &soundPitch, &effectSound, &effectChannel})
    if (list->empty()) list->push_back(0);
  out << "static const uint8_t music_sound_envelope[] = {\n" << cList(soundEnvelope) << "};\n";
  out << "static const uint8_t music_sound_release[] = {\n" << cList(soundRelease) << "};\n";
  out << "static const uint16_t music_sound_pitch[] = {\n" << cList(soundPitch) << "};\n";
  out << "static const uint8_t music_effect_sound[] = {\n" << cList(effectSound) << "};\n";
  out << "static const uint8_t music_effect_channel[] = {\n" << cList(effectChannel) << "};\n";
  dataBytes += (int)builder.sounds.entries.size() * 4 + (int)effects.size() * 2;
  out << kCTiaPlayer << "\n#endif\n";
  result.ok = true;
  result.text = indentWithTabs(out.str(), 4, false);
  result.dataBytes = dataBytes;
  result.sounds = (int)builder.sounds.entries.size();
  result.envelopes = (int)builder.envelopes.entries.size();
  result.releases = (int)builder.releases.entries.size();
  result.pitches = (int)builder.pitches.entries.size();
  return result;
}

std::string songSymbol(const std::string& name) {
  std::string symbol = effectSymbol(name);
  return "MUSIC_SONG_" + symbol.substr(13);
}

ExportResult exportSong(const Song& song, double audioClock, const ExportOptions& options) {
  return exportSongs({{song.name, &song}}, audioClock, options);
}

static bool sameFrames(const std::vector<TiaFrame>& left, const std::vector<TiaFrame>& right) {
  return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](const TiaFrame& first, const TiaFrame& second) {
    return first.control == second.control && first.frequency == second.frequency && first.volume == second.volume;
  });
}

// Every cell of a song becomes an event. The event can depend on what played before (the
// channel's instrument, the instrument of the note that sounds), so the song is walked as
// it plays: from the start, then again from the loop, until nothing changes.
static void walkSong(int songIndex, Builder& builder, Combined& combined) {
  const Song& song = *combined.songs[songIndex].song;
  builder.song = &song;
  const int tracks = std::min(combined.tracks, song.trackCount());  // the TIA track last, after the voices
  int channelInstrument[kMaxTracks] = {}, soundingInstrument[kMaxTracks] = {-1, -1, -1, -1, -1};
  std::string prefix = combined.songs.size() > 1 ? combined.songs[songIndex].name + ": " : "";
  auto walk = [&](int from) {
    bool changed = false;
    for (int entry = from; entry < (int)song.order.size(); entry++) {
      int patternIndex = song.order[entry];
      if (patternIndex < 0 || patternIndex >= (int)song.patterns.size()) continue;
      const Pattern& pattern = song.patterns[patternIndex];
      for (int row = 0; row < pattern.rows; row++)
        for (int channel = 0; channel < tracks; channel++) {
          const bool onTia = song.trackCells(channel) == kTiaTrack;
          const Cell& cell = pattern.cells[song.trackCells(channel)][row];
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
              event.code = 6 + builder.sound(instrument, cell.note, volumeVariant, releaseVariant, onTia);
            }
          } else if (cell.note == kNoteOff) {
            if (cell.effect == kEffectRelease && builder.valid(sounding)) event = {4, builder.release(sounding, cell.effectValue)};
            else event.code = 1;
          } else if (cell.effect == kEffectVolume && builder.valid(sounding)) {
            event = {2, builder.envelope(sounding, cell.effectValue)};
          } else if (cell.effect == kEffectRelease && builder.valid(sounding)) {
            event = {3, builder.release(sounding, cell.effectValue)};
          }
          auto key = std::make_tuple(songIndex, patternIndex, channel, row);
          auto found = combined.events.find(key);
          if (found == combined.events.end()) {
            combined.events[key] = event;
            changed = true;
          } else if (!(found->second == event)) {
            std::string name = pattern.name.empty() ? F("pattern {0}", {std::to_string(patternIndex)}) : pattern.name;
            builder.problem(prefix + F("{0}, channel {1}, row {2}: it plays differently depending on what came before; give the cell its instrument",
                                       {name, onTia ? std::string("TIA") : std::to_string(channel), std::to_string(row)}));
          }
        }
    }
    return changed;
  };
  walk(0);
  if (song.loopTo >= 0)
    for (int pass = 0; pass < 4 && walk(song.loopTo); pass++) {
    }
  // the song's entries in the shared order list, and the one that closes it
  combined.songStart.push_back((int)combined.order.size());
  for (int patternIndex : song.order) {
    OrderEntry entry;
    entry.song = songIndex;
    bool valid = patternIndex >= 0 && patternIndex < (int)song.patterns.size();
    entry.pattern = valid ? patternIndex : -1;
    entry.rows = valid ? song.patterns[patternIndex].rows : 1;
    entry.even = std::clamp(valid ? song.rowFrames(song.patterns[patternIndex], 0) : song.speed, 1, 255);
    entry.odd = std::clamp(valid ? song.rowFrames(song.patterns[patternIndex], 1) : song.rowFrames(1), 1, 255);
    if (entry.rows > 255) builder.problem(prefix + F("{0} has {1} rows: the player takes 255", {song.patterns[patternIndex].name.empty() ? F("pattern {0}", {std::to_string(patternIndex)}) : song.patterns[patternIndex].name, std::to_string(entry.rows)}));
    combined.order.push_back(entry);
  }
  OrderEntry closing;
  closing.song = songIndex;
  int own = (int)combined.order.size();
  closing.loopTo = song.loopTo >= 0 && song.loopTo < (int)song.order.size() ? combined.songStart.back() + song.loopTo : own;
  combined.order.push_back(closing);
  // sound effects; in a song of samples, those on the TIA's channel 1 become tables of
  // AUDC1, AUDF1 and AUDV1 frame by frame, played as the editor plays them (channel $FF).
  // An effect another song already has, the same, is kept once.
  for (auto& effect : song.effects) {
    std::string symbol = effectSymbol(effect.name);
    if (!builder.valid(effect.instrument)) {
      builder.problem(prefix + F("Sound effect {0}: its instrument does not exist", {effect.name}));
      continue;
    }
    std::pair<int, int> made;
    std::vector<TiaFrame> frames;
    bool onTia = song.samples() && effect.tia;
    if (onTia) {
      int index = (int)(&effect - song.effects.data());
      bool endless = false;
      frames = tiaEffectFrames(song, builder.clock, index, kMaxTiaEffectFrames, &endless);
      if (endless) builder.problem(prefix + F("Sound effect {0} on the TIA's channel 1 lasts more than {1} frames: its volume envelope must end on 0", {effect.name, std::to_string(kMaxTiaEffectFrames)}));
    } else {
      // The player lets an effect go only when its volume reaches 0: nothing plays its note off
      const Envelope* volume = song.instruments[effect.instrument].volumes.empty() ? nullptr : &song.instruments[effect.instrument].volumes[0];
      if ((!volume || volume->values.empty() || volume->loop >= 0 || volume->values.back() != 0))
        builder.problem(prefix + F("Sound effect {0} never ends and keeps its channel: its instrument's volume envelope must end on 0, with no loop", {effect.name}));
      made = {builder.sound(effect.instrument, effect.note, 0, 0), std::clamp(effect.channel, 0, combined.channels - 1)};
    }
    auto same = std::find_if(combined.effects.begin(), combined.effects.end(), [&](auto& existing) { return existing.first == symbol; });
    if (same != combined.effects.end()) {
      bool identical = onTia ? same->second.second == 0xFF && sameFrames(combined.tiaTables[same->second.first], frames) : same->second == made;
      if (!identical) builder.problem(prefix + F("Sound effect {0}: another one has the same name", {effect.name}));
      continue;
    }
    if (onTia) {
      combined.effects.push_back({symbol, {(int)combined.tiaTables.size(), 0xFF}});
      combined.tiaTables.push_back(frames);
    } else {
      combined.effects.push_back({symbol, made});
    }
  }
}

ExportResult exportSongs(const std::vector<NamedSong>& songs, double audioClock, const ExportOptions& options) {
  ExportResult result;
  const bool dpc = options.target == ExportTarget::DpcPlus;
  const bool cdfj = options.target == ExportTarget::Cdfj;
  const bool samplesTarget = options.target == ExportTarget::C || dpc || cdfj;
  if (songs.empty()) {
    result.problems.push_back(T("There is no song to export."));
    return result;
  }
  // the songs that can go together: the first one sets the voices and the mix
  Combined combined;
  std::set<std::string> songSymbols;
  for (auto& named : songs) {
    const Song& song = *named.song;
    std::string prefix = songs.size() > 1 ? named.name + ": " : "";
    if (song.instruments.empty() || song.order.empty()) {
      result.problems.push_back(prefix + T("The song has no instruments or no order list."));
      continue;
    }
    // C takes either kind; the songs of a folder are all of the first one's
    if (options.target == ExportTarget::C && !combined.songs.empty() && song.samples() != combined.songs[0].song->samples()) {
      result.problems.push_back(prefix + T("Left out: songs played together are all played as samples or all on the TIA."));
      continue;
    }
    if (options.target != ExportTarget::C && samplesTarget != song.samples()) {
      result.problems.push_back(prefix + (song.samples() ? T("A song played as samples is exported as C (ELF or CDFJ) or for DPC+.")
                                                         : T("CDFJ and DPC+ export are for songs played as samples; a TIA song is exported for DASM, batari Basic or C.")));
      continue;
    }
    if (!combined.songs.empty() && (song.channelCount() != combined.channels || (song.samples() && song.gain != combined.gain))) {
      result.problems.push_back(prefix + F("Left out: songs played together need the same voices and loudness as {0}", {combined.songs[0].name}));
      continue;
    }
    std::string symbol = songSymbol(named.name);
    if (!songSymbols.insert(symbol).second) {
      result.problems.push_back(prefix + F("Left out: another song is also called {0}", {symbol}));
      continue;
    }
    if (combined.songs.empty()) combined.channels = song.channelCount(), combined.gain = song.gain, combined.speed = song.speed, combined.swing = song.swing;
    if (song.patternSpeeds() || song.speed != combined.speed || song.swing != combined.swing) combined.patternSpeeds = true;
    combined.songs.push_back(named);
    combined.songSymbols.push_back(symbol);
  }
  if (combined.songs.empty()) return result;
  const int channels = combined.channels;
  bool anyTiaTrack = false;
  for (auto& named : combined.songs) anyTiaTrack = anyTiaTrack || named.song->hasTiaTrack();
  combined.tracks = channels + (anyTiaTrack && (options.target == ExportTarget::C || cdfj) ? 1 : 0);
  if (anyTiaTrack && dpc) result.problems.push_back(T("DPC+ does not play the TIA track yet: it is left out."));
  Builder builder{*combined.songs[0].song, audioClock, dpc};
  builder.problems = result.problems;
  for (auto& text : result.problems) builder.said.insert(text);
  for (int index = 0; index < (int)combined.songs.size(); index++) walkSong(index, builder, combined);
  bool tooLong = false;
  for (auto& entry : combined.order) tooLong = tooLong || entry.rows > 255;
  if (builder.tiaSounds.entries.size() > 250) builder.problem(F("{0} different sounds on the TIA track: the player takes 250", {std::to_string(builder.tiaSounds.entries.size())}));
  if (builder.sounds.entries.size() > 250) builder.problem(F("{0} different sounds: the player takes 250", {std::to_string(builder.sounds.entries.size())}));
  if (builder.releases.entries.size() > 64) builder.problem(F("{0} different releases: the player takes 64", {std::to_string(builder.releases.entries.size())}));
  if (builder.envelopes.entries.size() > 256) builder.problem(F("{0} different volume envelopes: the player takes 256", {std::to_string(builder.envelopes.entries.size())}));
  if (combined.order.size() > 255) builder.problem(combined.songs.size() > 1 ? T("The songs' order lists are longer than 255 entries together (each song takes one more).") : T("The order list is longer than 254 entries."));
  result.problems = builder.problems;
  if (builder.sounds.entries.size() > 250 || builder.tiaSounds.entries.size() > 250 || builder.releases.entries.size() > 64 || builder.envelopes.entries.size() > 256 || combined.order.size() > 255 || tooLong) return result;
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
  if (options.target == ExportTarget::C && !combined.songs[0].song->samples()) return writeCTia(combined, builder, options, result);
  if (options.target == ExportTarget::C || cdfj) return writeC(combined, builder, options, result);
  const auto& effects = combined.effects;
  const auto& tiaTables = combined.tiaTables;

  // the text
  std::ostringstream out;
  out << "; Music for the Atari 2600, made with VCSIDE's music editor: every song of this\n";
  out << "; folder, with one player. Generated from the .vcsmusic files: change those, not this.\n";
  out << "; Tuned for " << options.tv << ". The songs, for MusicPlay:\n" << songList(combined, ";");
  if (options.target == ExportTarget::BatariBasic) {
    out << ";\n; batari Basic: the player uses the variables " << options.ramStart << " and the 18 after it ("
        << options.ramStart << " to z if it is h): do not use them elsewhere. Call it with\n";
    out << ";     asm\n;     jsr MusicInit      (once)\n;     end\n";
    out << "; and, once per frame (before drawscreen):\n;     asm\n;     jsr MusicFrame\n;     end\n";
    out << "; another song:\n;     asm\n;     lda #MUSIC_SONG_x\n;     jsr MusicPlay\n;     end\n";
    out << "; and put 'inline <this file>' where the program never runs into it (after a goto).\n";
    out << "\nMUSIC_RAM = " << options.ramStart << "\n";
  } else if (dpc) {
    int ramBytes = 5 + 7 * channels + (tiaTables.empty() ? 0 : 3);
    out << ";\n; DPC+: include this file three times, with MUSIC_PART set before each:\n";
    out << ";   MUSIC_PART SET 1   in a bank of 6507 code: the player and the songs (after DPCplus.h)\n";
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
  out << "\nMUSIC_SPEED = " << std::clamp(combined.speed, 1, 255) << "\n";
  out << "MUSIC_SWING = " << std::clamp(combined.swing, 0, 255) << "\n";
  out << "MUSIC_PATTERN_SPEEDS = " << (combined.patternSpeeds ? 1 : 0) << "  ; patterns with speeds of their own\n";
  out << "MUSIC_ORDER_LENGTH = " << combined.order.size() << "  ; every song's entries and the one that closes it\n";
  out << "MUSIC_SONGS = " << combined.songs.size() << "\n";
  for (size_t index = 0; index < combined.songs.size(); index++) out << combined.songSymbols[index] << " = " << index << "\n";
  out << "MUSIC_CHANNELS = " << channels << "\n";
  out << "MUSIC_DPC = " << (dpc ? 1 : 0) << "\n";
  out << "MUSIC_TIA_EFFECTS = " << tiaTables.size() << "  ; sound effects on the TIA's channel 1\n";
  for (size_t index = 0; index < effects.size(); index++) out << effects[index].first << " = " << index << "\n";
  out << kPlayer;
  out << "\n; ---- the songs ----\n";
  // order list: a pattern's address, or (closing a song) the entry it loops to
  auto patternLabel = [&](int song, int patternIndex) { return "MusicPattern" + std::to_string(song) + "_" + std::to_string(patternIndex); };
  std::string lows, highs;
  std::vector<int> orderRows;
  for (size_t index = 0; index < combined.order.size(); index++) {
    const OrderEntry& entry = combined.order[index];
    if (entry.closing()) {
      lows += listSeparator(index) + std::to_string(entry.loopTo);
      highs += listSeparator(index) + "0";
    } else {
      // an order entry with no pattern plays one row of nothing: any pattern's first row would
      // not do, so it points at a row of zeros
      std::string label = entry.pattern >= 0 ? patternLabel(entry.song, entry.pattern) : "MusicNoRow";
      lows += listSeparator(index) + "<" + label;
      highs += listSeparator(index) + ">" + label;
    }
    orderRows.push_back(entry.rows);
  }
  out << "\nMusicOrderLo\n" << lows << "\nMusicOrderHi\n" << highs << "\nMusicOrderRows\n" << byteList(orderRows);
  out << "MusicSongStart\n" << byteList(combined.songStart);
  dataBytes += (int)combined.order.size() * 3 + (int)combined.songStart.size();
  if (combined.patternSpeeds) {
    std::vector<int> evenFrames, oddFrames;
    for (auto& entry : combined.order) evenFrames.push_back(entry.even), oddFrames.push_back(entry.odd);
    out << "\nMusicOrderSpeed\n" << byteList(evenFrames) << "\nMusicOrderSwing\n" << byteList(oddFrames);
    dataBytes += (int)combined.order.size() * 2;
  }
  // patterns (each one a song's order list uses, once)
  bool noRow = false;
  for (auto& entry : combined.order) noRow = noRow || (!entry.closing() && entry.pattern < 0);
  if (noRow) {
    out << "MusicNoRow\n" << byteList(std::vector<int>(channels, 0));
    dataBytes += channels;
  }
  for (auto& [song, patternIndex] : combined.patterns()) {
    const Pattern& pattern = combined.songs[song].song->patterns[patternIndex];
    std::vector<int> bytes = combined.patternBytes(song, patternIndex);
    std::string note = combined.songs.size() > 1 ? combined.songs[song].name + (pattern.name.empty() ? "" : ", " + pattern.name) : pattern.name;
    out << patternLabel(song, patternIndex) << (note.empty() ? "" : "  ; " + note) << "\n" << byteList(bytes);
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
    for (int volume = 0; volume < 16; volume++) levels.push_back(dpcLevel(volume, channels, combined.gain));
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
  result.text = indentWithTabs(out.str(), 4, true);  // tabs, as the templates: conditionals and code one tab in
  result.dataBytes = dataBytes;
  result.sounds = (int)builder.sounds.entries.size();
  result.envelopes = (int)builder.envelopes.entries.size();
  result.releases = (int)builder.releases.entries.size();
  result.pitches = (int)builder.pitches.entries.size();
  return result;
}

FolderExport exportFolder(const std::string& folder, ExportTarget target, double audioClock, const ExportOptions& options, const std::string& editedPath,
                          const Song* edited) {
  namespace fs = std::filesystem;
  FolderExport folderExport;
  folderExport.path = (fs::path(folder) / exportFileName(target)).string();
  std::vector<fs::path> files;
  std::error_code errorCode;
  for (auto& entry : fs::directory_iterator(folder, errorCode))
    if (entry.path().extension() == ".vcsmusic") files.push_back(entry.path());
  std::sort(files.begin(), files.end(), [](const fs::path& left, const fs::path& right) { return left.filename().string() < right.filename().string(); });
  std::vector<Song> read(files.size());
  std::vector<NamedSong> songs;
  std::vector<std::string> notes;
  for (size_t index = 0; index < files.size(); index++) {
    std::string stem = files[index].stem().string();
    bool isEdited = edited && !editedPath.empty() && fs::equivalent(files[index], fs::path(editedPath), errorCode);
    const Song* song = isEdited ? edited : nullptr;
    if (!song) {
      std::ifstream file(files[index], std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(file)), {}), error;
      if (!fromJson(text, &read[index], &error)) {
        notes.push_back(F("{0}: left out, it cannot be read ({1})", {stem, error}));
        continue;
      }
      song = &read[index];
    }
    ExportTarget its;
    if (!exportTargetOf(song->exportTo, &its)) continue;  // not exported
    if (exportFileName(its) != std::string(exportFileName(target))) continue;  // another file
    if (its != target) {
      notes.push_back(F("{0}: left out, it is exported for another kind of project", {stem}));
      continue;
    }
    songs.push_back({stem, song});
  }
  if (edited && editedPath.empty()) songs.push_back({edited->name, edited});
  folderExport.result = exportSongs(songs, audioClock, options);
  folderExport.result.problems.insert(folderExport.result.problems.begin(), notes.begin(), notes.end());
  for (auto& named : songs) folderExport.songs.push_back(named.name);
  return folderExport;
}

bool writeFolderExport(const FolderExport& folderExport, std::string* error) {
  if (!folderExport.result.ok) {
    if (error) *error = folderExport.result.problems.empty() ? std::string() : folderExport.result.problems[0];
    return false;
  }
  std::ofstream file(folderExport.path, std::ios::binary);
  if (!file || !(file << folderExport.result.text)) {
    if (error) *error = F("Cannot write {0}", {folderExport.path});
    return false;
  }
  return true;
}

}  // namespace vcs::music
