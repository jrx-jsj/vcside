// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Code editor widget drawn with Dear ImGui: syntax colouring, cycle gutter,
// diagnostics, hover help and completion.
#pragma once
#include <functional>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"

#include "folding.h"
#include "indentation.h"

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
  // Jumps to where a symbol is defined (F12 / Ctrl+click); false if not found.
  std::function<bool(const std::string& word)> gotoDefinition;
  // Whether gotoDefinition would find it (Ctrl/Cmd over a word underlines it as a link).
  std::function<bool(const std::string& word, int line)> hasDefinition;
  // A file a line names (include, incbin, #include...): whether it can be opened, and
  // opening it (Ctrl/Cmd+click on its name).
  std::function<bool(const std::string& name)> hasFile;
  std::function<void(const std::string& name)> openFile;
};

struct Match {
  int line, start, end;  // byte offsets
};

// Search options and the matches of a query in some lines (exposed for tests).
struct FindOptions {
  bool matchCase = false, wholeWord = false;
};
std::vector<Match> findMatches(const std::vector<std::string>& lines, const std::string& query, FindOptions options);

struct Coord {
  int line = 0, col = 0;  // col = byte offset
  bool operator==(const Coord& other) const { return line == other.line && col == other.col; }
  bool operator!=(const Coord& other) const { return !(*this == other); }
  bool operator<(const Coord& other) const { return line < other.line || (line == other.line && col < other.col); }
};

class TextEditor {
 public:
  TextEditor();
  void setText(const std::string& text);
  std::string text() const;
  // Replaces the whole text as one edit that can be undone (the cursor stays where it was).
  void replaceText(const std::string& text);
  const std::vector<std::string>& lines() const { return lines_; }

  void setHooks(EditorHooks hooks) { hooks_ = std::move(hooks); }
  void setMarkers(std::map<int, std::pair<bool, std::string>> markers) { markers_ = std::move(markers); }  // line -> (error?, msg)
  void setGutterWidthChars(int chars) { gutterChars_ = chars; }

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

  // Line editing
  void setCommentPrefix(const std::string& prefix) { commentPrefix_ = prefix; }
  void toggleComment();
  void duplicateLines();
  void moveLines(int delta);  // -1 up, +1 down
  void deleteLines();

  // Find / replace / go to line (bar at the top of the editor)
  void openFind(bool replace);
  void openGoto();
  void findNext(bool backwards = false);
  int replaceAll();
  bool replaceCurrent();
  void setFindQuery(const std::string& query) { findText_ = query; }
  void setReplaceText(const std::string& replacement) { replaceText_ = replacement; }
  const std::vector<Match>& matches() const { return matches_; }
  // Word under the cursor (for F12 / F1).
  std::string wordAtCursor() const;

  // Breakpoints (0-based lines): click in the left margin or F9. They follow the
  // text when lines are inserted or deleted above them.
  void toggleBreakpoint(int line);
  void setBreakpoints(const std::set<int>& lines);
  const std::set<int>& breakpoints();
  // True once after the breakpoints changed (toggled, or moved by an edit).
  bool takeBreakpointsChanged() { bool changed = bpChanged_; bpChanged_ = false; return changed; }
  // Folding: the regions come from the text (folding.h); a folded region shows only its
  // first line. Folds follow the text when lines are inserted or deleted above them, and
  // a region opens when the cursor goes into it.
  void setFoldSyntax(FoldSyntax syntax) { foldSyntax_ = syntax, regionsVersion_ = 0; }
  // Indentation: a level is what most of the file uses (indentation.h); `fallback` for a file
  // with no indented line yet. In C (FoldSyntax::C), Enter and braces indent as the code goes.
  void setDefaultIndent(IndentStyle fallback) { defaultIndent_ = fallback; }
  IndentStyle indentStyle() const { return detectIndentStyle(lines_, defaultIndent_); }
  // What the keys do, also for tests: Enter, typed text, Tab (Shift+Tab: `outdent`), Backspace.
  void newline();
  void typeText(const std::string& text);
  void indentKey(bool outdent);
  void backspace();
  bool foldAt(int line);    // the innermost region that starts at or holds the line
  bool unfoldAt(int line);  // the folded region that starts at or holds the line
  void foldAll();
  void unfoldAll();
  bool folded(int line);    // the line starts a folded region
  bool hiddenLine(int line);
  // Ctrl (Cmd) + mouse wheel over the editor: zoom steps asked for since the last call.
  int takeZoomSteps() { int steps = zoomSteps_; zoomSteps_ = 0; return steps; }

 private:
  struct Snapshot {
    std::vector<std::string> lines;
    Coord cursor;
  };
  void pushUndo(bool mergeTyping);
  void insertText(const std::string& inserted);
  void deleteSelection();
  void del();
  void moveCursor(Coord target, bool select);
  std::string selectedText() const;
  Coord clamp(Coord coord) const;
  int visualCol(int line, int byteCol) const;
  int byteCol(int line, int visCol) const;
  std::string wordAt(Coord coord, int* start = nullptr, int* end = nullptr) const;
  // The word last checked for a definition under Ctrl/Cmd, and the answer.
  std::string linkWord_;
  int linkLine_ = -1;
  bool linkOk_ = false;
  void handleKeyboard();
  void renderFindBar();
  void refreshMatches();
  void setSelection(Coord start, Coord end);
  void updateCompletion();
  void acceptCompletion();
  void syncBreakpoints();
  void syncFolds();         // regions for this text; folds moved with the edits
  void updateRows();        // which lines show, after the folds
  void reveal(int line);    // opens the folds that hide it
  const FoldRegion* regionAt(int first) const;

  std::vector<std::string> lines_{""};
  Coord cursor_, selStart_, selEnd_, anchor_;
  std::vector<Snapshot> undo_, redo_;
  bool lastWasTyping_ = false;
  bool dirty_ = false, changed_ = false;
  bool scrollToCursor_ = false, centerOnCursor_ = false;
  int tabSize_ = 8;
  int gutterChars_ = 0;
  EditorHooks hooks_;
  std::map<int, std::pair<bool, std::string>> markers_;
  std::set<int> bps_;
  std::vector<std::string> bpBase_;  // text the breakpoint lines refer to
  size_t bpVersion_ = 0;
  bool bpChanged_ = false;
  std::vector<Completion> completions_;
  int completionSel_ = 0;
  bool completionOpen_ = false;
  float charW_ = 7, lineH_ = 14;
  std::string commentPrefix_ = ";";
  int zoomSteps_ = 0;

  enum class Bar { None, Find, Replace, Goto } bar_ = Bar::None;
  bool focusBar_ = false, focusEditor_ = false;
  std::string findText_, replaceText_, gotoText_;
  FindOptions findOptions_;
  std::vector<Match> matches_;
  size_t matchesVersion_ = 0, textVersion_ = 1;
  std::string matchesQuery_;
  FindOptions matchesOptions_;
  int currentMatch_ = -1;

  FoldSyntax foldSyntax_ = FoldSyntax::None;
  IndentStyle defaultIndent_;
  std::vector<FoldRegion> regions_;
  size_t regionsVersion_ = 0, foldVersion_ = 0;
  std::set<int> folds_;               // first lines of the folded regions
  std::vector<std::string> foldBase_;  // the text the folds refer to
  std::vector<int> rowOf_, lineAt_;   // line -> visible row (hidden: its fold's row), row -> line
  bool rowsDirty_ = true;
};

}  // namespace vcs
