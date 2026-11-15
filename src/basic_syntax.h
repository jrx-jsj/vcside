// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// batari Basic in the editor: colouring, the label a line defines.
//
// A line that starts at column 0 begins with a label (or a line number); statements
// are indented. rem comments the rest of the line. Keywords are lowercase, as the
// compiler wants them; the TIA and RIOT registers can be written directly.
#pragma once
#include <string>
#include <vector>

#include "editor/text_editor.h"
#include "kb.h"

namespace vcs {

std::vector<Token> tokenizeBasic(const KnowledgeBase& kb, const std::string& line);
// The label defined at the start of the line ("main", "10", "player0:" is not one), or "".
std::string basicLabelOf(const std::string& line);

// TinyELF Basic (Stefan Lenz's Basic2 on the PlusCart and UnoCart): every line starts with
// its number; keywords and functions in any case (the listing has them in capitals); REM
// comments the rest of the line. The numbers after GOTO, GOSUB, THEN and ELSE are lines.
std::vector<Token> tokenizeTinyBasic(const std::string& line);
// Its keywords and functions, in capitals, from the keyword table in TinyELF's main.c.
const std::vector<std::string>& tinyBasicKeywords();

}  // namespace vcs
