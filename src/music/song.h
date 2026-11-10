// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// A song for the TIA, as the music editor keeps it in a .vcsmusic file (JSON).
//
// Two channels, one per TIA voice. Patterns hold a few rows of both channels and the
// order list plays them one after another. Instruments say how a note sounds frame by
// frame: melodic ones take their pitch from the note (the closest AUDC and AUDF the
// tuning finds), drums play fixed AUDC and AUDF values. Instruments with per-frame
// envelopes and a separate kind for percussion, and swing (alternating row lengths),
// are how Andre "Kylearan" Wichmann's TIATracker (2016) writes TIA music; an effect
// column per cell is how trackers have worked since Karsten Obarski's Ultimate
// Soundtracker (1987).
#pragma once
#include <map>
#include <string>
#include <vector>

#include "tuning.h"

namespace vcs::music {

constexpr int kChannels = 2;
constexpr int kNoNote = -1;   // an empty cell: the channel goes on as it was
constexpr int kNoteOff = -2;  // the note is released

// Values frame by frame. While the note is held they repeat from loop to the end
// (-1: they stay on the last one).
struct Envelope {
  std::vector<int> values;
  int loop = -1;
};

enum class PitchUnit { Cents, Audf };

// A note's pitch set by hand for one instrument: its AUDC and its AUDF frame by frame
// (the last one stays). It replaces both the tuning's choice and the pitch envelope.
struct NoteTuning {
  int control = 4;
  std::vector<int> frequencies;
};

struct Instrument {
  std::string name = "instrument";
  bool drum = false;
  // Melodic: the AUDC values its notes may use (tonal ones); the tuning picks per note.
  std::vector<int> controls = {4, 12};
  // Volume (0..15). The first is the usual one; the V effect picks another.
  std::vector<Envelope> volumes = {{{15, 13, 11, 10}, -1}};
  // After a note off, played once (empty: silent at once). The R effect picks another.
  std::vector<std::vector<int>> releases = {{6, 3, 1}};
  // Melodic: added to the note's pitch frame by frame (the last one stays). In cents
  // the closest AUDF is found for every note and frame; in AUDF steps it is added to
  // AUDF as it is (positive is lower), which is not the same interval on every note.
  std::vector<int> pitch;
  PitchUnit pitchUnit = PitchUnit::Cents;
  // Melodic: notes tuned by hand, by the note heard (after the song's transposition).
  std::map<int, NoteTuning> byHand;
  // Drum: AUDC and AUDF frame by frame (the last ones stay).
  std::vector<int> drumControl = {8}, drumFrequency = {4};
};

// The effect column: a letter and a value.
//   Vn  the note uses volume envelope n (with no note: the held note changes to it)
//   Rn  the channel's note is released with release n (a new note goes back to 0)
constexpr char kEffectVolume = 'V', kEffectRelease = 'R';

struct Cell {
  int note = kNoNote;   // MIDI note, kNoNote or kNoteOff
  int instrument = -1;  // index in Song::instruments; -1: the channel's last one
  char effect = 0;      // 0: none
  int effectValue = 0;  // 0..15
};

struct Pattern {
  std::string name;
  int rows = 32;
  std::vector<Cell> cells[kChannels];  // rows cells per channel
  void resize(int rowCount);
};

// A sound for the game to play when something happens (a jump, a shot): an instrument
// and a note on a channel. While it sounds, the song's notes on that channel wait.
struct SoundEffect {
  std::string name = "effect";
  int instrument = 0;
  int note = 60;    // as a note of the song (the transposition applies)
  int channel = 1;
};

struct SongTuningSettings {
  int transpose = 0;       // semitones added to every note when it is played
  int referenceCents = 0;  // A4 moved by this many cents
  TuningGoal goal = TuningGoal::WorstNote;
};

// What a melodic note writes on one frame, and how far that is from what was meant
// (the note plus the pitch envelope, if in cents).
struct PitchStep {
  bool playable = false;
  int control = 0, frequency = 0;
  double cents = 0;
};

struct Song {
  std::string name = "song";
  int speed = 6;   // video frames per row
  int swing = 0;   // frames of the odd rows (0: the same as speed)
  std::vector<Instrument> instruments;
  std::vector<Pattern> patterns;
  std::vector<int> order = {0};  // patterns in playing order
  int loopTo = 0;                // order entry to go back to at the end (-1: stop)
  std::vector<SoundEffect> effects;
  // Where the song is exported on every save: "" (nowhere), "dasm" or "bb" (batari
  // Basic), and for batari Basic the variable the player's RAM starts at.
  std::string exportTo;
  std::string exportRam = "h";
  SongTuningSettings tuning;

  int rowFrames(int row) const { return (row % 2 == 1 && swing > 0) ? swing : speed; }
  // The notes the song plays, with the AUDC values each one's instrument allows (melodic only).
  std::vector<SongNote> notes() const;
  // The notes heard with each instrument (after the transposition), for the tuning by hand.
  std::vector<int> notesOf(int instrument) const;
  // What a cell's note sounds as at its start: tuned by hand, or the closest pitch.
  NotePitch pitchOf(int note, const Instrument& instrument, double audioClock) const;
  // Frame by frame, what a note writes to AUDC and AUDF (at least one step; the last stays).
  std::vector<PitchStep> pitchSequence(int note, const Instrument& instrument, double audioClock) const;
};

Song blankSong();  // one pattern, a lead and a drum
std::string toJson(const Song& song);
bool fromJson(const std::string& text, Song* song, std::string* error);

}  // namespace vcs::music
