// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The .vcsproj in a tab: a form with the project's settings, or its JSON as text.
// Both edit the same file; switching shows the other one with the changes.
#pragma once
#include <string>

#include "editor/text_editor.h"
#include "kb.h"
#include "project.h"
#include "toolchain.h"

struct ImFont;

namespace vcs {

class ProjectForm {
 public:
  // Reads the file into the text editor and the form.
  bool load(const std::string& path, TextEditor& text, std::string* error);
  void render(TextEditor& text, ImFont* mono, const KnowledgeBase& kb, Toolchain& toolchain);
  bool dirty(const TextEditor& text) const { return dirty_ || text.dirty(); }
  // What to write: the form as JSON, or the text if it is shown (false if it does not read).
  bool contents(TextEditor& text, std::string* jsonText, std::string* error);
  void saved(TextEditor& text, const std::string& written);  // after writing: both clean
  bool showingText() const { return asText_; }
  bool showText(TextEditor& text, bool show);  // false: the text has an error and stays

 private:
  bool asText_ = false;
  bool dirty_ = false;   // the form has changes the file does not
  std::string path_, error_;
  Project project_;
  std::string includeText_;  // include folders, one per line, while editing
};

}  // namespace vcs
