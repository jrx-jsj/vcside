// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The TIA's sound, counter by counter, for the music editor to play what the console plays.
//
// Each of the two channels has three counters: a 5-bit divider that counts up to AUDF,
// a 5-bit noise shift register and a 4-bit pulse register. AUDC's low two bits choose
// how the noise register feeds back and when the pulse register holds; its high two
// bits choose how the pulse register feeds back. The channel's output is AUDV while
// the pulse register's low bit is 1, and 0 otherwise. With AUDC = 0 that bit settles
// at 1, so AUDV is heard as a level: the way samples are played.
//
// This counter-level description of the TIA's audio is Chris Brenner's ("TIA Sounding
// Off in the Digital Domain"), as Stella, 6502.ts and Gopher2600 implement it; the
// way the two channels mix (they share an output through resistors, so the sum is not
// linear) is his too, in Thomas Jentzsch's simplified form. The code here is VCSIDE's.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace vcs::music {

class TiaAudioChannel {
 public:
  uint8_t control = 0;    // AUDC, 4 bits
  uint8_t frequency = 0;  // AUDF, 5 bits
  uint8_t volume = 0;     // AUDV, 4 bits

  // One audio clock (two per scanline): the first half decides, the second shifts.
  void clock();
  // Whether the waveform is high now; the channel then sounds at its volume.
  bool high() const { return (pulseRegister_ & 1) != 0; }
  uint8_t level() const { return high() ? (uint8_t)(volume & 0x0f) : 0; }
  void reset() { *this = TiaAudioChannel(); }

 private:
  void decide();  // phase 0: reads the registers and steps the divider
  void shift();   // phase 1: shifts the noise and pulse registers when the divider said so

  uint8_t divider_ = 0, noiseRegister_ = 0, pulseRegister_ = 0;
  bool dividerFired_ = false;   // the divider reached AUDF on this clock
  bool noiseFeedback_ = false;  // the bit that enters the noise register
  bool noiseLowBit_ = false;    // the noise register's low bit, as read in phase 0
  bool pulseHolds_ = false;     // the pulse register does not shift on this clock
};

// The TIA's two channels as heard on its single output.
class TiaAudio {
 public:
  std::array<TiaAudioChannel, 2> channels;
  // One audio clock; returns the output, 0 (silence) to 1 (both channels at 15).
  float clock();
  void reset() { channels[0].reset(), channels[1].reset(); }
};

// The output for the sum of both channels' levels (0..30), from 0 to 1: the TIA mixes
// the two channels through resistors, so 15 + 15 is less than twice 15.
float mixLevels(int sumOfLevels);
// What one channel alone gives (level 0..15), on the same scale.
inline float channelLevel(int level) { return mixLevels(level); }

// Turns the audio clock rate (31.4 kHz) into the output rate (44.1 or 48 kHz): each
// output sample is the average of the TIA output over its span, so nothing is skipped.
class Resampler {
 public:
  Resampler(double inputRate, double outputRate) : step_(inputRate / outputRate) {}
  // Adds one TIA output value; any finished output samples are appended to output.
  void push(float value, std::vector<float>& output);

 private:
  double step_;            // input samples per output sample
  double filled_ = 0;      // how much of the current output sample has been covered
  double sum_ = 0;         // the weighted sum of the inputs in it
};

}  // namespace vcs::music
