// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// C and C++ in the editor (ELF, ACE, CDFJ and DPC+ projects): colouring.
//
// Keywords, types, the preprocessor, strings, character constants, numbers and comments,
// plus the 2600's register names (vcsLib writes them, e.g. vcsWrite5(COLUBK, ...)) and calls.
// A /* */ comment can run over several lines, so each line starts in a state the line
// before leaves: CLineCode or CLineBlockComment.
#pragma once
#include <string>
#include <vector>

#include "editor/text_editor.h"
#include "kb.h"

namespace vcs {

enum CLineState : int { CLineCode = 0, CLineBlockComment = 1 };

// The state at the end of the line, from the state at its start.
int cLineEndState(const std::string& line, int startState);
std::vector<Token> tokenizeC(const KnowledgeBase& kb, const std::string& line, int startState);

}  // namespace vcs
