// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Plays a song as the console would: once per video frame it works out what to write
// to AUDC, AUDF and AUDV for each channel. The players exported for the games follow
// the same rules, so what the editor plays is what the game plays.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "song.h"
#include "tia_audio.h"

namespace vcs::music {

struct FrameRegisters {
  uint8_t control[kChannels] = {}, frequency[kChannels] = {}, volume[kChannels] = {};
};

// Where the song is: the order entry and the row.
struct SongPosition {
  int order = 0, row = 0;
};

// Why a channel wrote what it wrote on a frame (for the register view).
struct VoiceTrace {
  bool active = false, released = false;
  int note = kNoNote, instrument = 0;
  int frame = 0;         // of the note (or of its release)
  int volumeVariant = 0, releaseVariant = 0;
  PitchStep pitch;       // melodic: the step played
};

class Player {
 public:
  // From an order entry and a row; audioClock: for the tuning (audioClockHz).
  void start(const Song& song, double audioClock, SongPosition from = {});
  void stop();
  // A new version of the song while it plays (an edit): the position is kept.
  void updateSong(const Song& song);
  // One note on a channel, outside the song (the editor's preview); held until noteOff.
  void noteOn(const Song& song, double audioClock, int channel, int note, int instrument);
  void noteOff(int channel);

  FrameRegisters nextFrame();
  bool playing() const { return playing_; }
  bool sounding() const;  // the song plays or a voice still sounds
  SongPosition position() const { return shown_; }  // the row being heard
  const VoiceTrace& trace(int channel) const { return traces_[channel]; }  // of the last frame

 private:
  struct Voice {
    int instrument = 0, note = kNoNote;
    bool active = false, released = false;
    int frame = 0, releaseFrame = 0;
    int volumeVariant = 0, releaseVariant = 0;
    std::vector<PitchStep> pitch;  // melodic: frame by frame, the last one stays
  };
  void playRow();
  void end();
  void trigger(Voice& voice, int note);
  void output(Voice& voice, int channel, FrameRegisters& registers);

  Song song_;
  double audioClock_ = 0;
  Voice voices_[kChannels];
  VoiceTrace traces_[kChannels];
  bool playing_ = false;
  SongPosition next_, shown_;
  int framesLeft_ = 0;  // in the current row
};

// Lines per video frame for a TV standard (262 or 312).
int linesPerFrame(const std::string& tv);

// The song turned into sound: the player, the TIA and the resampler together.
class Renderer {
 public:
  void setup(const std::string& tv, double outputRate);
  Player player;
  bool muted[kChannels] = {false, false};
  // Plays one video frame and appends its samples (from -1 to 1).
  void renderFrame(std::vector<float>& output);
  const FrameRegisters& lastRegisters() const { return registers_; }

 private:
  TiaAudio tia_;
  Resampler resampler_{31400, 48000};
  int clocksPerFrame_ = 524;
  float dcLevel_ = 0;  // the output's average, taken away so it centres on 0
  FrameRegisters registers_;
  std::vector<float> raw_;
};

// The whole song, once through (or maxFrames), at the output rate: for tests and WAV files.
std::vector<float> renderSong(const Song& song, const std::string& tv, double outputRate, int maxFrames);
bool writeWav(const std::string& path, const std::vector<float>& samples, int rate, std::string* error);

}  // namespace vcs::music
