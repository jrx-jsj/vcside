// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// A song as a DASM include for the console: VCSIDE's own player (CC0) and the song's
// tables, in one file. The player writes on every frame exactly what the editor's
// Player computes (tests/tests.cpp runs both and compares them write by write).
//
// What the game calls: MusicInit once, MusicFrame once per frame, MusicEffect with a
// sound effect's number in A. It needs MUSIC_RAM: 19 bytes of zero page.
//
// A song played as samples (Song::target "samples") is exported as C instead, for ELF
// cartridges: <name>.music.h with VCSIDE's C player (CC0), which works out the frame as
// the editor's Player does and mixes the voices into one AUDV0 level per scanline as
// SampleMixer does (tests compile it and compare both, level by level).
#pragma once
#include <string>
#include <vector>

#include "song.h"

namespace vcs::music {

enum class ExportTarget { Dasm, BatariBasic, C };  // C: a song played as samples, for ELF

struct ExportOptions {
  ExportTarget target = ExportTarget::Dasm;
  // batari Basic: the variable the player's 19 bytes start at ("h" uses h to z).
  // DASM: empty, the program defines MUSIC_RAM before the include.
  std::string ramStart;
  std::string tv = "NTSC";  // what the tuning was worked out for (said in the header)
};

struct ExportResult {
  bool ok = false;
  std::string text;                   // the .music.asm (or .music.h)
  std::vector<std::string> problems;  // what could not be exported as it plays in the editor
  int dataBytes = 0;                  // the song's tables (the player is about 400 bytes more)
  int sounds = 0, envelopes = 0, releases = 0, pitches = 0;
};

constexpr int kMusicRamBytes = 19;

ExportResult exportSong(const Song& song, double audioClock, const ExportOptions& options);

// The sound effects' constants in the export: MUSIC_EFFECT_<NAME> (letters, digits, _).
std::string effectSymbol(const std::string& name);

}  // namespace vcs::music
