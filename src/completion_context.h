// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// What the editor's completion knows from the knowledge base beyond names (docs/design-assistant.md, "AI completion", steps 1
// and 2, which need no model): where on the line the word is, so that an operand is not offered instructions and a store is
// not offered a register that can only be read; and the recipes with holes as snippets, when the first word of a line names
// what one does ("fire", "jump", "joystick").
#pragma once
#include <string>
#include <vector>

#include "kb.h"

namespace vcs {

struct LineContext {
  bool firstWord = true;   // nothing but blanks (or a label) before the word: an instruction or a directive goes here
  std::string mnemonic;    // else, the instruction the word is an operand of, lower case ("" if none)
};
// line: the whole line; prefix: the word being typed (taken as the last place it occurs).
LineContext lineContext(const KnowledgeBase& kb, const std::string& line, const std::string& prefix);

// A register as the operand of this instruction: a store wants one that can be written, a load or a test one that can be read.
bool registerFits(const Register& reg, const std::string& mnemonic);

// "$0C R": where it is and what can be done with it, for the popup.
std::string registerDetail(const Register& reg);

struct RecipeSnippet {
  std::string name;    // "fire-button-action/once-per-press"
  std::string title;   // what it does
  std::string text;    // the code, the slots as comments to replace
};
// The recipes with holes whose words start with the prefix (three letters at least), as code ready to insert: labels with a prefix
// that is free in `fileText`, instructions indented with `indent`, each slot a comment that says what goes there, and a first
// comment with the RAM bytes it needs. 2600 assembly only.
std::vector<RecipeSnippet> recipeSnippets(const KnowledgeBase& kb, const std::string& prefix, const std::string& fileText, const std::string& indent);

}  // namespace vcs
