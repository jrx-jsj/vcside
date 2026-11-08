// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// Code editor widget drawn with Dear ImGui: syntax colouring, cycle gutter,
// diagnostics, hover help and completion.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"

namespace vcs {

enum class TokenKind { Text, Comment, String, Number, Mnemonic, IllegalMnemonic, UnstableMnemonic, Register, Directive, Label, Symbol };

struct Token {
  int start, end;  // byte offsets in the line
  TokenKind kind;
};

struct Completion {
  std::string word, detail;
  TokenKind kind = TokenKind::Text;
};

struct EditorHooks {
  // Colours one line.
  std::function<std::vector<Token>(const std::string&)> tokenize;
  // Text shown in the gutter for a line (e.g. "3" or "4/5" cycles); empty = nothing.
  std::function<std::string(int line, const std::string&)> gutter;
  // Help for the word under the mouse; empty = no tooltip.
  std::function<std::string(const std::string& word, const std::string& line)> hover;
  // Completion candidates for a prefix.
  std::function<std::vector<Completion>(const std::string& prefix, const std::string& line)> complete;
};

struct Coord {
  int line = 0, col = 0;  // col = byte offset
  bool operator==(const Coord& o) const { return line == o.line && col == o.col; }
  bool operator!=(const Coord& o) const { return !(*this == o); }
  bool operator<(const Coord& o) const { return line < o.line || (line == o.line && col < o.col); }
};

class TextEditor {
 public:
  TextEditor();
  void setText(const std::string& text);
  std::string text() const;
  const std::vector<std::string>& lines() const { return lines_; }

  void setHooks(EditorHooks h) { hooks_ = std::move(h); }
  void setMarkers(std::map<int, std::pair<bool, std::string>> m) { markers_ = std::move(m); }  // line -> (error?, msg)
  void setGutterWidthChars(int n) { gutterChars_ = n; }

  // Draws the editor filling the available region. Returns true if the text changed.
  bool render(const char* id, ImFont* font);

  bool hasSelection() const { return selStart_ != selEnd_; }
  // Lines covered by the selection (or the cursor line).
  std::pair<int, int> selectedLines() const;
  Coord cursor() const { return cursor_; }
  void goTo(int line, int col = 0);
  bool dirty() const { return dirty_; }
  void markSaved() { dirty_ = false; }

  void undo();
  void redo();
  void copy();
  void cut();
  void paste();
  void selectAll();
  void selectLines(int first, int last);  // 0-based, inclusive

 private:
  struct Snapshot {
    std::vector<std::string> lines;
    Coord cursor;
  };
  void pushUndo(bool mergeTyping);
  void insertText(const std::string& s);
  void deleteSelection();
  void backspace();
  void del();
  void newline();
  void moveCursor(Coord c, bool select);
  std::string selectedText() const;
  Coord clamp(Coord c) const;
  int visualCol(int line, int byteCol) const;
  int byteCol(int line, int visCol) const;
  std::string wordAt(Coord c, int* start = nullptr, int* end = nullptr) const;
  void handleKeyboard();
  void updateCompletion();
  void acceptCompletion();

  std::vector<std::string> lines_{""};
  Coord cursor_, selStart_, selEnd_, anchor_;
  std::vector<Snapshot> undo_, redo_;
  bool lastWasTyping_ = false;
  bool dirty_ = false, changed_ = false;
  bool scrollToCursor_ = false;
  int tabSize_ = 8;
  int gutterChars_ = 0;
  EditorHooks hooks_;
  std::map<int, std::pair<bool, std::string>> markers_;
  std::vector<Completion> completions_;
  int completionSel_ = 0;
  bool completionOpen_ = false;
  float charW_ = 7, lineH_ = 14;
};

}  // namespace vcs
