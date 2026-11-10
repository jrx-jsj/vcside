// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Reads a TIATracker song (.ttt, Andre "Kylearan" Wichmann's tracker for the TIA, 2016).
//
// A .ttt is JSON: 7 instruments (an AUDC, and per frame an AUDF offset and a volume, with
// a sustain loop and a release), 15 percussion sounds (AUDC, AUDF and volume per frame),
// patterns of notes whose value is an AUDF (not a musical note), and a sequence of
// patterns per channel with jumps. Only the format is read; no TIATracker code is used.
//
// In VCSIDE a pattern holds both channels, so the two sequences are laid out side by side
// and cut where either starts a pattern. Each AUDF a melodic instrument plays becomes the
// closest musical note, tuned by hand to that AUDC and AUDF, so the song sounds as it did.
#pragma once
#include <string>
#include <vector>

#include "song.h"

namespace vcs::music {

struct ImportResult {
  bool ok = false;
  std::string error;                  // when !ok
  std::vector<std::string> warnings;  // what could not be kept exactly
};

// The note names are measured with the clock of the song's TV mode (its "tvmode").
ImportResult importTiaTracker(const std::string& text, const std::string& fallbackName, Song* song);

}  // namespace vcs::music
