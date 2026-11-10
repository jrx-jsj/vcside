// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "export.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "../i18n.h"

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
; RAM: MUSIC_RAM and the 18 bytes after it, in zero page.
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

MusicInit SUBROUTINE
    ldx #18
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
    bit MusicPitchIndex1
    bmi .keep1
    jsr MusicNoteOff1
.keep1
    lda #$FF
    sta MusicOrder
    bne .voices
.row
    jsr MusicPlayRow
.wait
    dec MusicWait
.voices
    jsr MusicVoice0
    jmp MusicVoice1

MusicPlayRow SUBROUTINE
    ldy #0
    jsr MusicEvent0
    jsr MusicEvent1
    tya
    clc
    adc MusicPointer
    sta MusicPointer
    bcc .length
    inc MusicPointer+1
.length
    ldx #MUSIC_SPEED
  IF MUSIC_SWING > 0
    lda MusicRow
    lsr
    bcc .even
    ldx #MUSIC_SWING
.even
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
    bne .channel1
    jsr MusicStart0
    lda #$80
    sta MusicPitchIndex0
    rts
.channel1
    jsr MusicStart1
    lda #$80
    sta MusicPitchIndex1
    rts

; One channel's routines; {1} is the channel.
    MAC MUSIC_CHANNEL

; A row's event for the channel: Y is the offset in the row and moves past it.
MusicEvent{1} SUBROUTINE
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

; This frame's AUDC, AUDF and AUDV for the channel.
MusicVoice{1} SUBROUTINE
    lda MusicState{1}
    bmi .sounding
.silent
    lda #0
    sta AUDV{1}
    sta AUDC{1}
    sta AUDF{1}
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

    ENDM

    MUSIC_CHANNEL 0
    MUSIC_CHANNEL 1
)PLAYER";

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
  Builder(const Song& songToExport, double audioClock) : song(songToExport), clock(audioClock) {}
  const Song& song;
  double clock;
  std::vector<std::string> problems;
  std::set<std::string> said;
  Pool<std::pair<std::vector<int>, int>> envelopes;  // values, loop
  Pool<std::vector<int>> releases;
  Pool<std::vector<int>> pitches;                     // length (+$80 muted), then AUDC and AUDF pairs
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

ExportResult exportSong(const Song& song, double audioClock, const ExportOptions& options) {
  ExportResult result;
  Builder builder{song, audioClock};
  if (song.instruments.empty() || song.order.empty()) {
    result.problems.push_back(T("The song has no instruments or no order list."));
    return result;
  }
  // Every cell becomes an event. The event can depend on what played before (the
  // channel's instrument, the instrument of the note that sounds), so the song is walked
  // as it plays: from the start, then again from the loop, until nothing changes.
  std::map<std::tuple<int, int, int>, Event> events;  // pattern, channel, row
  int channelInstrument[kChannels] = {0, 0}, soundingInstrument[kChannels] = {-1, -1};
  auto walk = [&](int from) {
    bool changed = false;
    for (int entry = from; entry < (int)song.order.size(); entry++) {
      int patternIndex = song.order[entry];
      if (patternIndex < 0 || patternIndex >= (int)song.patterns.size()) continue;
      const Pattern& pattern = song.patterns[patternIndex];
      for (int row = 0; row < pattern.rows; row++)
        for (int channel = 0; channel < kChannels; channel++) {
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
  // sound effects
  std::vector<std::pair<std::string, std::pair<int, int>>> effects;  // symbol, sound and channel
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
    effects.push_back({symbol, {builder.sound(effect.instrument, effect.note, 0, 0), std::clamp(effect.channel, 0, 1)}});
  }
  if (builder.sounds.entries.size() > 250) builder.problem(F("{0} different sounds: the player takes 250", {std::to_string(builder.sounds.entries.size())}));
  if (builder.releases.entries.size() > 64) builder.problem(F("{0} different releases: the player takes 64", {std::to_string(builder.releases.entries.size())}));
  if (builder.envelopes.entries.size() > 256) builder.problem(F("{0} different volume envelopes: the player takes 256", {std::to_string(builder.envelopes.entries.size())}));
  if (song.order.size() > 254) builder.problem(T("The order list is longer than 254 entries."));
  result.problems = builder.problems;
  if (builder.sounds.entries.size() > 250 || builder.releases.entries.size() > 64 || builder.envelopes.entries.size() > 256 || song.order.size() > 254) return result;

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
  } else {
    out << ";\n; Define MUSIC_RAM (19 bytes of zero page) before including this file.\n";
    out << "\n  IFNCONST MUSIC_RAM\n    ERR ; MUSIC_RAM is not defined: 19 bytes of zero page for the music player\n  ENDIF\n";
  }
  int dataBytes = 0;
  std::vector<int> orderRows;
  for (int patternIndex : song.order) orderRows.push_back(patternIndex >= 0 && patternIndex < (int)song.patterns.size() ? song.patterns[patternIndex].rows : 1);
  out << "\nMUSIC_SPEED = " << std::clamp(song.speed, 1, 255) << "\n";
  out << "MUSIC_SWING = " << std::clamp(song.swing, 0, 255) << "\n";
  out << "MUSIC_ORDER_LENGTH = " << song.order.size() << "\n";
  out << "MUSIC_LOOP_TO = " << (song.loopTo >= 0 ? std::to_string(song.loopTo) : "MUSIC_ORDER_LENGTH  ; stops at the end") << "\n";
  for (size_t index = 0; index < effects.size(); index++) out << effects[index].first << " = " << index << "\n";
  out << kPlayer;
  out << "\n; ---- the song ----\n";
  // order list
  out << "\nMusicOrderLo\n";
  std::string lows, highs;
  for (size_t entry = 0; entry < song.order.size(); entry++) {
    lows += std::string(entry ? "," : "    .byte ") + "<MusicPattern" + std::to_string(song.order[entry]);
    highs += std::string(entry ? "," : "    .byte ") + ">MusicPattern" + std::to_string(song.order[entry]);
  }
  out << lows << "\nMusicOrderHi\n" << highs << "\nMusicOrderRows\n" << byteList(orderRows);
  dataBytes += (int)song.order.size() * 3;
  // patterns (each one used by the order list, once)
  std::set<int> written;
  for (int patternIndex : song.order) {
    if (patternIndex < 0 || patternIndex >= (int)song.patterns.size() || !written.insert(patternIndex).second) continue;
    const Pattern& pattern = song.patterns[patternIndex];
    std::vector<int> bytes;
    for (int row = 0; row < pattern.rows; row++)
      for (int channel = 0; channel < kChannels; channel++) {
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
    pitchLows += std::string(index ? "," : "    .byte ") + "<MusicPitchData" + std::to_string(pitchIndex);
    pitchHighs += std::string(index ? "," : "    .byte ") + ">MusicPitchData" + std::to_string(pitchIndex);
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
  // envelopes: length, loop ($FF none), values; releases: length, $FF, values
  auto pointerTables = [&](const char* name, const char* dataName, size_t count) {
    std::string low = std::string(name) + "Lo\n", high = std::string(name) + "Hi\n";
    for (size_t index = 0; index < count; index++) {
      low += std::string(index ? "," : "    .byte ") + "<" + dataName + std::to_string(index);
      high += std::string(index ? "," : "    .byte ") + ">" + dataName + std::to_string(index);
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
