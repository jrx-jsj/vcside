// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Knowledge-base grounding: finds the names the knowledge base knows (registers, opcodes,
// cartridge formats, TV standards, batari Basic words) in what the model is about to see
// and returns their entries in a compact form, so even a small model that cannot call tools
// answers from the project's sources instead of from its memory of the 2600.
#pragma once
#include <string>
#include <vector>

#include "kb.h"

namespace vcs::assistant {

struct GroundingContext {
  std::string format;        // the project's cartridge format id ("F8"), if any
  std::string tv;            // "NTSC", "PAL"...
  bool batariBasic = false;  // plain words such as "playfield" are batari Basic's
};

struct GroundedEntry {
  std::string kind;   // "TIA register", "6502 instruction", "cartridge format", "TV standard", "batari Basic"
  std::string id;     // RESP0, LDA, F8, NTSC, pfcolors
  std::string text;   // the compact entry that goes in the prompt
  bool verified = true;
  int score = 0;
};

// userText: what the user wrote. codeText: the selection, the code around the cursor and the
// build errors; names there match in any case, in the user's text only the unambiguous ones
// (UPPERCASE registers and mnemonics). At most maxEntries entries and maxChars characters.
std::vector<GroundedEntry> groundText(const KnowledgeBase& kb, const std::string& userText, const std::string& codeText,
                                      const GroundingContext& context, size_t maxEntries = 3, size_t maxChars = 2400);

// The entries as one block for the prompt (empty if none), marked as reference data.
std::string groundingBlock(const std::vector<GroundedEntry>& entries);

}  // namespace vcs::assistant
