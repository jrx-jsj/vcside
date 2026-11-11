// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The documentation cards as Markdown: the text of a register or a recipe with its bit table, steps and code as fenced blocks.
// Pure text, so it can be tested; the Documentation panel draws it.
#pragma once
#include <string>
#include <vector>

#include "kb.h"

namespace vcs {

// A card to open from another card: topic is "R:INPT4", "I:LDA" or "K:read-controls".
struct DocLink {
  std::string topic, label;
};

// The body of a register's card: description, the bit table, timing, known issues and an example (no title line: the panel draws it).
std::string registerMarkdown(const Register& reg);
// The body of a recipe's card: summary, numbered steps, example and note.
std::string techniqueMarkdown(const Technique& technique);

// "See also" of a register: the ones its data names, the registers its text mentions, and the recipes that use it.
std::vector<DocLink> registerSeeAlso(const KnowledgeBase& kb, const Register& reg);
// "See also" of a recipe: the registers and instructions it relates to, and the other recipes that share them.
std::vector<DocLink> techniqueSeeAlso(const KnowledgeBase& kb, const Technique& technique);

}  // namespace vcs
