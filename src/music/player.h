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
  // TIA: what each channel writes. Samples: volume is the voice's (0..15) for the frame.
  uint8_t control[kMaxChannels] = {}, frequency[kMaxChannels] = {}, volume[kMaxChannels] = {};
  // Samples: whether the voice sounds (its phase moves), the phase step per scanline, its
  // instrument (the waveform), and whether a note started since the last frame (the
  // phase and the noise register start again).
  bool sounding[kMaxChannels] = {}, restart[kMaxChannels] = {};
  uint32_t phaseStep[kMaxChannels] = {};
  int instrument[kMaxChannels] = {};
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
  bool effect = false;   // a sound effect, not the song
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
  // A sound effect of the song (Song::effects) on its channel: the music's notes there
  // wait until it has ended, as in the exported player.
  void playEffect(const Song& song, double audioClock, int effect);

  FrameRegisters nextFrame();
  bool playing() const { return playing_; }
  bool sounding() const;  // the song plays or a voice still sounds
  SongPosition position() const { return shown_; }  // the row being heard
  const VoiceTrace& trace(int channel) const { return traces_[channel]; }  // of the last frame
  const Song& song() const { return song_; }

 private:
  struct Voice {
    int instrument = 0, note = kNoNote;  // the instrument of the note sounding
    bool active = false, released = false, locked = false;  // locked: a sound effect
    int frame = 0, volumeIndex = 0, releaseFrame = 0;
    int volumeVariant = 0, releaseVariant = 0, releaseUsed = 0;  // releaseUsed: set at the note off
    std::vector<PitchStep> pitch;  // melodic: frame by frame, the last one stays
    std::vector<uint32_t> steps;   // samples: the phase step frame by frame, the last one stays
    bool restart = false;          // samples: a note started since the last frame
  };
  void playRow();
  void end();
  void trigger(Voice& voice, int note);
  void output(Voice& voice, int channel, FrameRegisters& registers);

  Song song_;
  double audioClock_ = 0;
  Voice voices_[kMaxChannels];
  int channelInstruments_[kMaxChannels] = {};  // for the channel's next notes
  VoiceTrace traces_[kMaxChannels];
  bool playing_ = false;
  SongPosition next_, shown_;
  int framesLeft_ = 0;  // in the current row
};

// Lines per video frame for a TV standard (262 or 312).
int linesPerFrame(const std::string& tv);

// Samples: the voices mixed into one AUDV0 level per scanline, as the exported C player
// does it, with the same integer arithmetic (tests compare them value by value). Each
// voice adds its waveform's level (0..15) times its volume (0..15); the sum, scaled by
// the song's gain, is in quarters of a level (0..63) and goes through a table to the AUDV
// whose output comes closest, since the TIA's output is not proportional to AUDV.
class SampleMixer {
 public:
  void reset();
  // One frame: `lines` levels appended to out. muted: voices left out (may be null).
  void render(const Song& song, const FrameRegisters& registers, int lines, std::vector<uint8_t>& out, const bool* muted = nullptr);

 private:
  uint32_t phase_[kMaxChannels] = {};
  uint16_t noise_[kMaxChannels] = {1, 1, 1, 1};
  uint8_t lastStep_[kMaxChannels] = {};
};
// The scale from the sum of voices to quarters of a level, in 1/65536.
uint32_t mixFactor(int gain, int voices);
// For quarters of a level 0..63, the AUDV (0..15) whose output comes closest.
const std::array<uint8_t, 64>& levelTable();

// The song turned into sound: the player, the TIA and the resampler together.
class Renderer {
 public:
  void setup(const std::string& tv, double outputRate);
  Player player;
  bool muted[kMaxChannels] = {};
  // Plays one video frame and appends its samples (from -1 to 1).
  void renderFrame(std::vector<float>& output);
  const FrameRegisters& lastRegisters() const { return registers_; }

 private:
  TiaAudio tia_;
  SampleMixer mixer_;
  std::vector<uint8_t> lineLevels_;
  int linesPerFrame_ = 262;
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
