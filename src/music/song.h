// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// A song for the TIA, as the music editor keeps it in a .vcsmusic file (JSON).
//
// Two channels, one per TIA voice. Patterns hold a few rows of both channels and the
// order list plays them one after another. Instruments say how a note sounds frame by
// frame: melodic ones take their pitch from the note (the closest AUDC and AUDF the
// tuning finds), drums play fixed AUDC and AUDF values. Instruments with per-frame
// envelopes and a separate kind for percussion, and swing (alternating row lengths),
// are how Andre "Kylearan" Wichmann's TIATracker (2016) writes TIA music.
#pragma once
#include <string>
#include <vector>

#include "tuning.h"

namespace vcs::music {

constexpr int kChannels = 2;
constexpr int kNoNote = -1;   // an empty cell: the channel goes on as it was
constexpr int kNoteOff = -2;  // the note is released

struct Instrument {
  std::string name = "instrument";
  bool drum = false;
  // Melodic: the AUDC values its notes may use (tonal ones); the tuning picks per note.
  std::vector<int> controls = {4, 12};
  // Volume (0..15) frame by frame from the note's start. While the note is held it
  // repeats from loopStart to the end (-1: it stays on the last value).
  std::vector<int> volume = {15, 13, 11, 10};
  int loopStart = -1;
  std::vector<int> release = {6, 3, 1};  // after a note off; empty: silent at once
  // Melodic: added to AUDF frame by frame (the last one stays); positive is lower.
  std::vector<int> pitch;
  // Drum: AUDC and AUDF frame by frame (the last ones stay).
  std::vector<int> drumControl = {8}, drumFrequency = {4};
};

struct Cell {
  int note = kNoNote;   // MIDI note, kNoNote or kNoteOff
  int instrument = -1;  // index in Song::instruments; -1: the channel's last one
};

struct Pattern {
  std::string name;
  int rows = 32;
  std::vector<Cell> cells[kChannels];  // rows cells per channel
  void resize(int rowCount);
};

struct SongTuningSettings {
  int transpose = 0;       // semitones added to every note when it is played
  int referenceCents = 0;  // A4 moved by this many cents
  TuningGoal goal = TuningGoal::WorstNote;
};

struct Song {
  std::string name = "song";
  int speed = 6;   // video frames per row
  int swing = 0;   // frames of the odd rows (0: the same as speed)
  std::vector<Instrument> instruments;
  std::vector<Pattern> patterns;
  std::vector<int> order = {0};  // patterns in playing order
  int loopTo = 0;                // order entry to go back to at the end (-1: stop)
  SongTuningSettings tuning;

  int rowFrames(int row) const { return (row % 2 == 1 && swing > 0) ? swing : speed; }
  // The notes the song plays, with the AUDC values each one's instrument allows (melodic only).
  std::vector<SongNote> notes() const;
  // What a cell's note sounds as with the song's tuning (transposed, the closest pitch).
  NotePitch pitchOf(int note, const Instrument& instrument, double audioClock) const;
};

Song blankSong();  // one pattern, a lead and a drum
std::string toJson(const Song& song);
bool fromJson(const std::string& text, Song* song, std::string* error);

}  // namespace vcs::music
