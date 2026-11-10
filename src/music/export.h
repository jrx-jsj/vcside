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
//
// Such a song with up to 3 voices can also go to a DPC+ cartridge (Darrell Spice Jr. and
// Fred Quimby's, as in Stay Frosty 2), whose ARM adds 3 voices of 32-sample waveforms
// into one amplitude the 6507 copies to AUDV0 on every line: the same 6502 player, with
// WAVEFORMx and NOTEx for each voice, every waveform copied at a few volumes into the
// display data, and the song's own frequency table.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "song.h"

namespace vcs::music {

// C: a song played as samples, for ELF (the ARM mixes it). DpcPlus: for DASM, the voices
// played by a DPC+ cartridge. Cdfj: C for a CDFJ cartridge's ARM code, which plays them.
enum class ExportTarget { Dasm, BatariBasic, C, DpcPlus, Cdfj };

// The longest sound effect on the TIA's channel 1, in frames (its table counts frames + 1 in a byte).
constexpr int kMaxTiaEffectFrames = 254;

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

constexpr int kMusicRamBytes = 19;  // DPC+: 5 + 7 per voice (26 for 3)

// DPC+: what a voice's volume (0..15) becomes. DPC+ adds the voices' samples with no
// volume of its own and AUDV0 takes 4 bits, so each voice gets at most 15 / voices
// (5 with 3 voices), scaled by the song's gain; the waveform is copied at each level.
int dpcLevel(int volume, int voices, int gain);
int dpcMaxLevel(int voices);
// The 32 samples (0..15) of an instrument's waveform; noise is 32 steps of its register.
std::array<uint8_t, 32> dpcWave(const Instrument& instrument);
// DPC+ counts a voice's phase 20000 times a second: the step per scanline turned into it.
uint32_t dpcFrequency(uint32_t stepPerLine, double audioClock);

ExportResult exportSong(const Song& song, double audioClock, const ExportOptions& options);

// What Song::exportTo names: "dasm", "bb", "c", "dpc" or "cdfj"; false for anything else.
bool exportTargetOf(const std::string& name, ExportTarget* target);
// The export's file ending: ".music.h" for C and CDFJ, ".music.asm" for the rest.
const char* exportExtension(ExportTarget target);

// The sound effects' constants in the export: MUSIC_EFFECT_<NAME> (letters, digits, _).
std::string effectSymbol(const std::string& name);

}  // namespace vcs::music
