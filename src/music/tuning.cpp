// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "tuning.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>

#include "../i18n.h"

namespace vcs::music {

const std::vector<Distortion>& distortions() {
  // Periods as Ron Fries' TIASOUND documents them, checked by the tests against the
  // counter-level synthesis in tia_audio.cpp. One differs: TIASOUND gives AUDC 15 a
  // period of 186, but the counters repeat it every 93 clocks (as for AUDC 14, with a
  // different waveform), and the emulators agree with the counters.
  static const std::vector<Distortion> list = {
      {"square", N_("Square"), {4, 5}, 2, true},
      {"div6", N_("Square ÷6"), {12, 13}, 6, true},
      {"poly4", N_("Buzz (4-bit)"), {1}, 15, true},
      {"div31", N_("Pulse ÷31"), {6, 10}, 31, true},
      {"poly5", N_("5-bit"), {7, 9}, 31, true},
      {"div93", N_("Low ÷93"), {14}, 93, true},
      {"poly5div6", N_("5-bit ÷6"), {15}, 93, true},
      {"rumble", N_("Rumble"), {2, 3}, 465, false},
      {"noise", N_("Noise (9-bit)"), {8}, 511, false},
  };
  return list;
}

const Distortion* distortionOf(int control) {
  for (auto& distortion : distortions())
    for (int value : distortion.controls)
      if (value == control) return &distortion;
  return nullptr;
}

double audioClockHz(const std::string& tv) {
  // NTSC: 3,579,545.45 Hz colour clock. PAL (and PAL60, the same console): 3,546,894 Hz.
  // SECAM consoles are taken as PAL ones (not checked).
  bool ntsc = tv == "NTSC" || tv == "NTSC50" || tv == "PAL-M";
  return (ntsc ? 3579545.4545 : 3546894.0) / 114.0;
}

double pitchHz(double audioClock, int period, int frequency) { return audioClock / ((frequency + 1) * (double)period); }

double noteNumber(double hz, double referenceCents) { return 69.0 + 12.0 * std::log2(hz / 440.0) - referenceCents / 100.0; }

std::string noteName(int note) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  int octave = (int)std::floor(note / 12.0) - 1;
  return std::string(names[((note % 12) + 12) % 12]) + std::to_string(octave);
}

NotePitch closestPitch(int note, double audioClock, const std::vector<int>& allowedControls, double referenceCents) {
  NotePitch best;
  best.note = note;
  double bestDistance = 1e9;
  for (int control : allowedControls) {
    const Distortion* distortion = distortionOf(control);
    if (!distortion || !distortion->tonal) continue;
    for (int frequency = 0; frequency < 32; frequency++) {
      double hz = pitchHz(audioClock, distortion->period, frequency);
      double cents = (noteNumber(hz, referenceCents) - note) * 100.0;
      if (std::fabs(cents) < bestDistance) {
        bestDistance = std::fabs(cents);
        best.control = control, best.frequency = frequency, best.hz = hz, best.cents = cents;
      }
    }
  }
  best.playable = bestDistance <= 50.0;
  return best;
}

SongTuning measureSong(const std::vector<SongNote>& notes, double audioClock, int transpose, int referenceCents) {
  SongTuning tuning;
  tuning.transpose = transpose, tuning.referenceCents = referenceCents;
  // a song repeats its notes: each different one is measured once
  std::map<std::pair<int, std::vector<int>>, NotePitch> measured;
  double sum = 0;
  int playable = 0;
  for (auto& songNote : notes) {
    auto key = std::make_pair(songNote.note + transpose, songNote.allowedControls);
    auto found = measured.find(key);
    if (found == measured.end()) found = measured.emplace(key, closestPitch(key.first, audioClock, key.second, referenceCents)).first;
    const NotePitch& pitch = found->second;
    tuning.pitches.push_back(pitch);
    if (!pitch.playable) {
      tuning.unplayable++;
      continue;
    }
    playable++;
    sum += std::fabs(pitch.cents);
    tuning.worstCents = std::max(tuning.worstCents, std::fabs(pitch.cents));
  }
  tuning.averageCents = playable ? sum / playable : 0;
  return tuning;
}

SongTuning tuneSong(const std::vector<SongNote>& notes, double audioClock, TuningGoal goal, int maxTranspose) {
  // Only the different notes matter for the search; each is weighted by how often it is played.
  std::map<std::pair<int, std::vector<int>>, int> counts;
  for (auto& songNote : notes) counts[{songNote.note, songNote.allowedControls}]++;
  std::vector<SongNote> distinct;
  std::vector<int> weights;
  for (auto& [key, count] : counts) distinct.push_back({key.first, key.second}), weights.push_back(count);

  auto score = [&](int transpose, int referenceCents) {
    int unplayable = 0, playable = 0;
    double worst = 0, sum = 0;
    for (size_t index = 0; index < distinct.size(); index++) {
      NotePitch pitch = closestPitch(distinct[index].note + transpose, audioClock, distinct[index].allowedControls, referenceCents);
      if (!pitch.playable) {
        unplayable += weights[index];
        continue;
      }
      playable += weights[index];
      worst = std::max(worst, std::fabs(pitch.cents));
      sum += std::fabs(pitch.cents) * weights[index];
    }
    double average = playable ? sum / playable : 0;
    // the fewest unplayable notes first, then the goal, then the other measure, then the smallest change
    return std::make_tuple(unplayable, goal == TuningGoal::WorstNote ? worst : average, goal == TuningGoal::WorstNote ? average : worst,
                           std::abs(transpose) * 100 + std::abs(referenceCents));
  };
  int bestTranspose = 0, bestReference = 0;
  auto best = score(0, 0);
  for (int transpose = -maxTranspose; transpose <= maxTranspose; transpose++)
    for (int referenceCents = -50; referenceCents <= 50; referenceCents++) {
      auto candidate = score(transpose, referenceCents);
      if (candidate < best) best = candidate, bestTranspose = transpose, bestReference = referenceCents;
    }
  return measureSong(notes, audioClock, bestTranspose, bestReference);
}

}  // namespace vcs::music
