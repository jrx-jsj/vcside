// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Checks an answer against what exists (docs/design-assistant.md, "Names the model made up"): the
// registers and instructions of the knowledge base, and the names in the user's own files. A name in
// the code of the answer that is neither is listed, because small models invent registers
// (a PADDLE that the 2600 does not have) and write them with the same confidence as real ones.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "kb.h"

namespace vcs::assistant {

// UPPERCASE names (three or more characters) in the code of the answer, inside ``` blocks and `inline
// code`, that are not a register, an instruction, a directive, a cartridge format, a TV standard or a
// batari Basic word of the knowledge base (only in a batari Basic project: `paddle` is a word there and a made-up register in DASM), not a name the project knows (inProject, lower case) and not
// defined by the answer itself (NAME = value, NAME EQU value, a label in column 0). In order of
// appearance, each once, at most `limit`.
std::vector<std::string> unknownNames(const std::string& answer, const KnowledgeBase& kb, const std::function<bool(const std::string& lowerCaseWord)>& inProject,
                                      size_t limit = 6, bool batariBasic = false);

}  // namespace vcs::assistant
