// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// What goes along with a question, as chips the user can see and change (docs/design-assistant.md,
// "Context"): the open file (or the selected lines), the build errors, and any file or folder of the
// project added by typing @. This is the logic only, so it is tested without the panel.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace vcs::assistant {

struct ContextChip {
  enum Kind { File, Selection, Errors, Path } kind = File;
  std::string label;  // "main.asm", "main.asm:12-20", "Build errors", "src/kernel.asm"
  std::string path;   // File, Selection and Path: relative to the project
  bool automatic = false;
};

// What is open in the editor now.
struct ContextSources {
  std::string fileRelative;          // "" = no text file open
  bool hasSelection = false;
  int selectionFirst = 0, selectionLast = 0;  // 1-based
  bool hasErrors = false;
  std::string errorsKey;             // changes when the errors do
};

class ContextSet {
 public:
  // Call every frame. The file, the selection and the errors are chips of their own; one the user
  // removed stays removed until its source changes (another file, another selection, other errors).
  void sync(const ContextSources& sources);
  // The chips in order: the selection or the file, the errors, then what was added with @.
  std::vector<ContextChip> chips(const std::string& errorsLabel) const;
  void remove(size_t index);
  // Adds project files (relative paths); one already there is not added twice.
  void addFiles(const std::vector<std::string>& relativePaths);
  void clearAdded() { added_.clear(); }
  size_t addedCount() const { return added_.size(); }

 private:
  ContextSources sources_;
  std::string fileKey_, selectionKey_, errorsKey_;
  bool fileRemoved_ = false, selectionRemoved_ = false, errorsRemoved_ = false;
  std::vector<std::string> added_;
  bool hasFileChip() const;
  bool hasSelectionChip() const;
  bool hasErrorsChip() const;
};

// Files and folders of the project for the @ list, best matches first. A folder ends in '/'.
// Hidden folders, build/, files that may hold secrets and binaries are never listed.
std::vector<std::string> listProjectEntries(const std::string& projectDir, const std::string& filter, size_t limit);

// The text files in a folder (and its subfolders, three levels down), by path. total is how many
// there are; only the first maxFiles are returned.
std::vector<std::string> filesInFolder(const std::string& projectDir, const std::string& folderRelative, size_t maxFiles, size_t* total);

// What the chips come to, within a budget of characters: the selection or the file, the errors and the
// added files, in that order, each cut to what is left. notes says what was cut or could not be read.
// Something that did not go whole: a text cut to lines first..last of total, or a file that could not be read.
struct ContextCut {
  std::string label;
  int firstLine = 0, lastLine = 0, totalLines = 0;
  std::string reason;  // set when nothing was sent (a file the sandbox refuses, or no room left)
};
struct GatheredContext {
  std::string fileName, fileText;      // the open file as a chip
  std::string selectionLabel, selection;
  std::string errors;
  std::vector<std::pair<std::string, std::string>> extraFiles;  // path, text
  std::vector<ContextCut> notes;
  std::string summary;                 // "main.asm:12-20, build errors, constants.h"
};
struct ContextText {  // what the editor holds
  std::string fileText, selection, errors;
  int cursorLine = 0;  // 1-based; a file too long for the budget is cut around it, not at its start
};
GatheredContext gatherContext(const std::vector<ContextChip>& chips, const ContextText& text, const std::string& projectDir, size_t budgetChars);

}  // namespace vcs::assistant
