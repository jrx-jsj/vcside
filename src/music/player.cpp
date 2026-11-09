// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../i18n.h"

namespace vcs::music {

void Player::start(const Song& song, double audioClock, SongPosition from) {
  song_ = song;
  audioClock_ = audioClock;
  for (auto& voice : voices_) voice = Voice();
  next_ = from;
  if (next_.order < 0 || next_.order >= (int)song_.order.size()) next_ = {};
  shown_ = next_;
  framesLeft_ = 0;
  playing_ = true;
}

void Player::stop() {
  playing_ = false;
  for (auto& voice : voices_) voice.active = false;
}

void Player::updateSong(const Song& song) {
  song_ = song;
  if (next_.order >= (int)song_.order.size()) next_ = {};
  for (auto& voice : voices_)
    if (voice.instrument >= (int)song_.instruments.size()) voice.active = false;
}

void Player::noteOn(const Song& song, double audioClock, int channel, int note, int instrument) {
  if (!playing_) song_ = song, audioClock_ = audioClock;
  Voice& voice = voices_[channel];
  voice.instrument = instrument;
  trigger(voice, note);
}

void Player::noteOff(int channel) {
  Voice& voice = voices_[channel];
  if (voice.active && !voice.released) voice.released = true, voice.releaseFrame = 0;
}

bool Player::sounding() const {
  if (playing_) return true;
  for (auto& voice : voices_)
    if (voice.active) return true;
  return false;
}

void Player::trigger(Voice& voice, int note) {
  voice.active = voice.instrument >= 0 && voice.instrument < (int)song_.instruments.size();
  voice.released = false;
  voice.frame = voice.releaseFrame = 0;
  voice.silent = false;
  if (!voice.active) return;
  const Instrument& instrument = song_.instruments[voice.instrument];
  if (instrument.drum) return;
  NotePitch pitch = song_.pitchOf(note, instrument, audioClock_);
  voice.silent = !pitch.playable;  // no allowed AUDC reaches it: it is not heard
  voice.control = pitch.control, voice.frequency = pitch.frequency;
}

// The song is over: what still sounds goes into its release.
void Player::end() {
  playing_ = false;
  for (int channel = 0; channel < kChannels; channel++) noteOff(channel);
}

void Player::playRow() {
  if (next_.order >= (int)song_.order.size()) {
    end();
    return;
  }
  int patternIndex = song_.order[next_.order];
  if (patternIndex < 0 || patternIndex >= (int)song_.patterns.size()) {
    end();
    return;
  }
  const Pattern& pattern = song_.patterns[patternIndex];
  shown_ = next_;
  for (int channel = 0; channel < kChannels; channel++) {
    if (next_.row >= (int)pattern.cells[channel].size()) continue;
    const Cell& cell = pattern.cells[channel][next_.row];
    Voice& voice = voices_[channel];
    if (cell.instrument >= 0) voice.instrument = cell.instrument;
    if (cell.note >= 0) trigger(voice, cell.note);
    else if (cell.note == kNoteOff) noteOff(channel);
  }
  framesLeft_ = std::max(1, song_.rowFrames(next_.row));
  // the next row: the pattern's end goes to the next order entry, the song's end loops or stops
  if (++next_.row >= pattern.rows) {
    next_.row = 0;
    if (++next_.order >= (int)song_.order.size()) next_.order = song_.loopTo >= 0 ? song_.loopTo : (int)song_.order.size();
  }
}

static int envelopeValue(const std::vector<int>& values, int loopStart, int frame) {
  if (values.empty()) return 0;
  if (frame < (int)values.size()) return values[frame];
  if (loopStart >= 0 && loopStart < (int)values.size()) return values[loopStart + (frame - loopStart) % ((int)values.size() - loopStart)];
  return values.back();
}

static int heldValue(const std::vector<int>& values, int frame, int fallback) {
  if (values.empty()) return fallback;
  return values[std::min(frame, (int)values.size() - 1)];
}

void Player::output(Voice& voice, int channel, FrameRegisters& registers) {
  registers.control[channel] = registers.frequency[channel] = registers.volume[channel] = 0;
  if (!voice.active) return;
  const Instrument& instrument = song_.instruments[voice.instrument];
  int volume;
  if (!voice.released) {
    // an envelope that has ended on 0 with no loop is over
    bool ended = voice.frame >= (int)instrument.volume.size() && (instrument.loopStart < 0 || instrument.loopStart >= (int)instrument.volume.size());
    if (ended && (instrument.volume.empty() || instrument.volume.back() == 0)) {
      voice.active = false;
      return;
    }
    volume = envelopeValue(instrument.volume, instrument.loopStart, voice.frame);
  } else {
    if (voice.releaseFrame >= (int)instrument.release.size()) {
      voice.active = false;
      return;
    }
    volume = instrument.release[voice.releaseFrame++];
  }
  if (instrument.drum) {
    registers.control[channel] = (uint8_t)heldValue(instrument.drumControl, voice.frame, 8);
    registers.frequency[channel] = (uint8_t)heldValue(instrument.drumFrequency, voice.frame, 0);
  } else {
    registers.control[channel] = (uint8_t)voice.control;
    registers.frequency[channel] = (uint8_t)std::clamp(voice.frequency + heldValue(instrument.pitch, voice.frame, 0), 0, 31);
  }
  registers.volume[channel] = voice.silent ? 0 : (uint8_t)volume;
  voice.frame++;
}

FrameRegisters Player::nextFrame() {
  if (playing_ && framesLeft_ <= 0) playRow();
  if (framesLeft_ > 0) framesLeft_--;
  FrameRegisters registers;
  for (int channel = 0; channel < kChannels; channel++) output(voices_[channel], channel, registers);
  return registers;
}

int linesPerFrame(const std::string& tv) { return (tv == "PAL" || tv == "SECAM" || tv == "NTSC50") ? 312 : 262; }

void Renderer::setup(const std::string& tv, double outputRate) {
  clocksPerFrame_ = linesPerFrame(tv) * 2;
  resampler_ = Resampler(audioClockHz(tv), outputRate);
  tia_.reset();
}

void Renderer::renderFrame(std::vector<float>& output) {
  registers_ = player.nextFrame();
  for (int channel = 0; channel < kChannels; channel++) {
    TiaAudioChannel& voice = tia_.channels[channel];
    voice.control = registers_.control[channel];
    voice.frequency = registers_.frequency[channel];
    voice.volume = muted[channel] ? 0 : registers_.volume[channel];
  }
  raw_.clear();
  for (int clockIndex = 0; clockIndex < clocksPerFrame_; clockIndex++) resampler_.push(tia_.clock(), raw_);
  for (float value : raw_) {
    // the TIA's output never goes below 0; a slow average taken away keeps it centred
    dcLevel_ += (value - dcLevel_) * 0.0005f;
    output.push_back((value - dcLevel_) * 0.8f);
  }
}

std::vector<float> renderSong(const Song& song, const std::string& tv, double outputRate, int maxFrames) {
  Renderer renderer;
  renderer.setup(tv, outputRate);
  Song once = song;
  once.loopTo = -1;
  renderer.player.start(once, audioClockHz(tv));
  std::vector<float> samples;
  for (int frame = 0; frame < maxFrames && renderer.player.sounding(); frame++) renderer.renderFrame(samples);
  return samples;
}

bool writeWav(const std::string& path, const std::vector<float>& samples, int rate, std::string* error) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    if (error) *error = F("Cannot write {0}", {path});
    return false;
  }
  auto put32 = [&](uint32_t value) { uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)}; std::fwrite(bytes, 1, 4, file); };
  auto put16 = [&](uint16_t value) { uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)}; std::fwrite(bytes, 1, 2, file); };
  uint32_t dataBytes = (uint32_t)samples.size() * 2;
  std::fwrite("RIFF", 1, 4, file), put32(36 + dataBytes), std::fwrite("WAVEfmt ", 1, 8, file);
  put32(16), put16(1), put16(1), put32((uint32_t)rate), put32((uint32_t)rate * 2), put16(2), put16(16);
  std::fwrite("data", 1, 4, file), put32(dataBytes);
  for (float sample : samples) put16((uint16_t)(int16_t)std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767));
  bool written = std::fclose(file) == 0;
  if (!written && error) *error = F("Cannot write {0}", {path});
  return written;
}

}  // namespace vcs::music
