// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// How in tune the TIA can play a note, and the tuning that suits a whole song best.
//
// A TIA tone is audio clock / ((AUDF + 1) × period of the distortion), as Ron Fries'
// TIASOUND documents it. The dividers are integers, so a note is only near some AUDF
// value. Paul Slocum's guide (2003) tuned sets of notes against each other rather than
// against A440; tuneSong does the same for a song: it looks for the transposition and
// the reference pitch that leave its notes least out of tune.
#pragma once
#include <string>
#include <vector>

namespace vcs::music {

// A family of AUDC values that give the same waveform.
struct Distortion {
  const char* id;            // "square", "div6"...
  const char* name;          // for people (to translate)
  std::vector<int> controls; // the AUDC values that give it
  int period;                // audio clocks per waveform cycle when AUDF = 0
  bool tonal;                // has a pitch (the noises have none worth tuning)
};
const std::vector<Distortion>& distortions();
const Distortion* distortionOf(int control);  // nullptr for AUDC 0 and 11 (a level, no tone)

// Audio clocks per second (two per scanline) for a TV standard: the TIA's clock / 114.
double audioClockHz(const std::string& tv);

double pitchHz(double audioClock, int period, int frequency);  // frequency = AUDF, 0..31
// Equal-tempered note number of a frequency (MIDI: 69 = A4) with A4 moved by referenceCents.
double noteNumber(double hz, double referenceCents = 0);
std::string noteName(int note);  // "C4", "F#5"

// How the TIA plays one note.
struct NotePitch {
  int note = 0;           // the note asked for (MIDI)
  bool playable = false;  // some allowed AUDC and AUDF falls within half a semitone
  int control = 0, frequency = 0;
  double hz = 0;
  double cents = 0;       // how far off: positive = sharp
};
// The closest TIA pitch for a note among the allowed AUDC values (tonal ones only).
NotePitch closestPitch(int note, double audioClock, const std::vector<int>& allowedControls, double referenceCents = 0);

struct SongNote {
  int note;                       // MIDI number as written in the song
  std::vector<int> allowedControls;  // what its instrument may use
};
enum class TuningGoal { WorstNote, Average };
struct SongTuning {
  int transpose = 0;           // semitones added to every note
  int referenceCents = 0;      // A4 moved by this many cents
  int unplayable = 0;          // notes no allowed AUDC can reach
  double worstCents = 0, averageCents = 0;  // over the playable notes, absolute values
  std::vector<NotePitch> pitches;  // one per song note, transposed
};
// Measures the song as it is: no transposition, A440 moved by referenceCents.
SongTuning measureSong(const std::vector<SongNote>& notes, double audioClock, int transpose = 0, int referenceCents = 0);
// Tries transpositions from -maxTranspose to +maxTranspose semitones and references from
// -50 to +50 cents; keeps the one with the fewest unplayable notes and then the smallest
// worst note or average, as asked (ties: the smallest change).
SongTuning tuneSong(const std::vector<SongNote>& notes, double audioClock, TuningGoal goal, int maxTranspose = 6);

}  // namespace vcs::music
