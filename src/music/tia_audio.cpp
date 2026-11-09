// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "tia_audio.h"

namespace vcs::music {

void TiaAudioChannel::decide() {
  // What the previous firing of the divider prepared is decided now, from the registers
  // as they are, and used by the shift that follows the next firing.
  if (dividerFired_) {
    noiseLowBit_ = (noiseRegister_ & 0x01) != 0;
    int noiseMode = control & 0x03;
    if (noiseMode == 2) pulseHolds_ = (noiseRegister_ & 0x1e) != 0x02;  // shifts once per noise cycle (÷31)
    else if (noiseMode == 3) pulseHolds_ = !noiseLowBit_;               // shifts when the noise says so
    else pulseHolds_ = false;
    if (noiseMode == 0) {
      // with the high bits at 0 the register fills with ones; otherwise the noise and
      // pulse registers together make the 9-bit sequence
      noiseFeedback_ = ((pulseRegister_ ^ noiseRegister_) & 0x01) != 0 || (noiseRegister_ == 0 && pulseRegister_ == 0x0a) ||
                       (control & 0x0c) == 0;
    } else {
      bool tap2 = (noiseRegister_ & 0x04) != 0, tap0 = (noiseRegister_ & 0x01) != 0;
      noiseFeedback_ = (tap2 != tap0) || noiseRegister_ == 0;  // the 5-bit sequence, never stuck at 0
    }
  }
  dividerFired_ = divider_ == frequency;
  divider_ = (divider_ == frequency || divider_ == 0x1f) ? 0 : (uint8_t)(divider_ + 1);
}

void TiaAudioChannel::shift() {
  if (!dividerFired_) return;
  bool pulseFeedback = false;
  switch (control >> 2) {
    case 0: {  // the 4-bit sequence (or, with AUDC = 0, nothing: the output settles high)
      bool bit1 = (pulseRegister_ & 0x02) != 0, bit0 = (pulseRegister_ & 0x01) != 0;
      pulseFeedback = bit1 != bit0 && pulseRegister_ != 0x0a && (control & 0x03) != 0;
      break;
    }
    case 1: pulseFeedback = (pulseRegister_ & 0x08) == 0; break;  // square: ÷2, ÷6 with the hold
    case 2: pulseFeedback = !noiseLowBit_; break;                 // follows the noise register
    case 3: pulseFeedback = !((pulseRegister_ & 0x02) != 0 || (pulseRegister_ & 0x0e) == 0); break;  // ÷6
  }
  noiseRegister_ >>= 1;
  if (noiseFeedback_) noiseRegister_ |= 0x10;
  if (!pulseHolds_) {
    pulseRegister_ = (uint8_t)(~(pulseRegister_ >> 1) & 0x07);
    if (pulseFeedback) pulseRegister_ |= 0x08;
  }
}

void TiaAudioChannel::clock() {
  decide();
  shift();
}

float mixLevels(int sumOfLevels) {
  // Brenner's mixing, as Jentzsch simplified it: out = sum / 30 × (30 + 30) / (30 + sum).
  if (sumOfLevels <= 0) return 0;
  if (sumOfLevels > 30) sumOfLevels = 30;
  return 2.0f * (float)sumOfLevels / (30.0f + (float)sumOfLevels);
}

float TiaAudio::clock() {
  channels[0].clock();
  channels[1].clock();
  return mixLevels(channels[0].level() + channels[1].level());
}

void Resampler::push(float value, std::vector<float>& output) {
  double remaining = 1.0;  // this input sample spans one unit of input time
  while (filled_ + remaining >= step_) {
    double part = step_ - filled_;
    sum_ += value * part;
    output.push_back((float)(sum_ / step_));
    remaining -= part;
    filled_ = 0, sum_ = 0;
  }
  sum_ += value * remaining;
  filled_ += remaining;
}

}  // namespace vcs::music
