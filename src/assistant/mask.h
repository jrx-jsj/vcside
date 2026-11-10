// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Selective translation (docs/design-assistant.md, "Working in English"): the prose of a text is
// translated and the code is not. Before a text goes to be translated, the code in it (fenced blocks,
// `inline code`, lines of assembly pasted without a fence) and the names in it (registers, labels,
// file names, hex numbers) are replaced by markers like [[3]]; after, the markers are replaced by what
// they stood for. A translation that lost or doubled a marker is refused, never guessed at.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace vcs::assistant {

struct MaskedText {
  std::string text;                // with [[1]], [[2]]... in place of the code and the names
  std::vector<std::string> spans;  // what each marker stands for (marker N is spans[N-1])
  bool hasProse() const;           // any letters left that need translating
};

// isMnemonic: does the word name a 6502 instruction (any case)? Used to tell pasted assembly from prose.
MaskedText maskCode(const std::string& text, const std::function<bool(const std::string&)>& isMnemonic);

// The translation with every marker put back. ok is false when a marker is missing, doubled or invented; then the
// returned text is the translation as it came.
std::string unmaskCode(const std::string& translated, const std::vector<std::string>& spans, bool* ok);

// A rough guess whether prose is English, so that a question already in English is not sent to be translated:
// true when it has no letters outside ASCII and more English function words than Spanish, French or German ones.
bool looksEnglish(const std::string& prose);

}  // namespace vcs::assistant
