// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// A change to a file as the model writes it in a chat answer (docs/design-assistant.md, "Changes in
// the chat"): a fenced block named "edit <path>" holding search-and-replace sections,
//
//   ```edit src/main.asm
//   <<<<<<< SEARCH
//   the text as it is in the file
//   =======
//   the text as it should be
//   >>>>>>> REPLACE
//   ```
//
// A block that is not like that is an example and gets no Apply. Where the change goes is found in
// the real file (tools.cpp: edit_file), never guessed.
#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace vcs::assistant {

struct EditSection {
  std::string search, replace;  // lines joined with '\n', no trailing newline; an empty search creates a file
};

struct EditBlock {
  std::string path;
  std::vector<EditSection> sections;
};

enum class EditParse { NotAnEdit, Edit, Broken };
// info: what follows the opening fence ("edit src/main.asm"); body: what is inside it.
// Broken: it was meant as an edit but is cut off or has no path; *problem says what.
EditParse parseEditBlock(const std::string& info, const std::string& body, EditBlock* block, std::string* problem);

// The arguments of the edit_file tool for a block: {"path", "edits": [...]}, or {"path", "content"} for a new file.
nlohmann::json editArguments(const EditBlock& block);

}  // namespace vcs::assistant
