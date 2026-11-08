// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "text_editor.h"

#include "imgui_stdlib.h"
#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace vcs {

static ImU32 colorOf(TokenKind k) {
  switch (k) {
    case TokenKind::Comment: return IM_COL32(106, 153, 85, 255);
    case TokenKind::String: return IM_COL32(206, 145, 120, 255);
    case TokenKind::Number: return IM_COL32(181, 206, 168, 255);
    case TokenKind::Mnemonic: return IM_COL32(86, 156, 214, 255);
    case TokenKind::IllegalMnemonic: return IM_COL32(197, 134, 192, 255);
    case TokenKind::UnstableMnemonic: return IM_COL32(244, 71, 71, 255);
    case TokenKind::Register: return IM_COL32(78, 201, 176, 255);
    case TokenKind::Directive: return IM_COL32(220, 220, 170, 255);
    case TokenKind::Label: return IM_COL32(156, 220, 254, 255);
    default: return IM_COL32(212, 212, 212, 255);
  }
}

static bool isWordChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.' || (unsigned char)c >= 0x80; }

TextEditor::TextEditor() {}

void TextEditor::setText(const std::string& text) {
  lines_.clear();
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r') cur.pop_back();
      lines_.push_back(cur);
      cur.clear();
    } else cur += c;
  }
  if (!cur.empty() && cur.back() == '\r') cur.pop_back();
  lines_.push_back(cur);
  cursor_ = selStart_ = selEnd_ = anchor_ = {};
  undo_.clear();
  redo_.clear();
  dirty_ = false;
}

std::string TextEditor::text() const {
  std::string s;
  for (size_t i = 0; i < lines_.size(); i++) {
    s += lines_[i];
    if (i + 1 < lines_.size()) s += '\n';
  }
  return s;
}

Coord TextEditor::clamp(Coord c) const {
  c.line = std::clamp(c.line, 0, (int)lines_.size() - 1);
  c.col = std::clamp(c.col, 0, (int)lines_[c.line].size());
  // never stop inside a UTF-8 sequence
  while (c.col > 0 && c.col < (int)lines_[c.line].size() && ((unsigned char)lines_[c.line][c.col] & 0xC0) == 0x80) c.col--;
  return c;
}

int TextEditor::visualCol(int line, int byteCol) const {
  const std::string& l = lines_[line];
  int v = 0;
  for (int i = 0; i < byteCol && i < (int)l.size(); i++) {
    unsigned char c = l[i];
    if (c == '\t') v = (v / tabSize_ + 1) * tabSize_;
    else if ((c & 0xC0) != 0x80) v++;
  }
  return v;
}

int TextEditor::byteCol(int line, int visCol) const {
  const std::string& l = lines_[line];
  int v = 0, i = 0;
  while (i < (int)l.size()) {
    unsigned char c = l[i];
    int w = c == '\t' ? (v / tabSize_ + 1) * tabSize_ - v : 1;
    if (v + w / 2.0f >= visCol + 0.5f && v + w > visCol) {
      if (visCol - v < (w + 1) / 2) break;
    }
    if (v >= visCol) break;
    v += w;
    i++;
    while (i < (int)l.size() && ((unsigned char)l[i] & 0xC0) == 0x80) i++;
  }
  return i;
}

std::string TextEditor::wordAt(Coord c, int* start, int* end) const {
  const std::string& l = lines_[c.line];
  int s = c.col, e = c.col;
  while (s > 0 && isWordChar(l[s - 1])) s--;
  while (e < (int)l.size() && isWordChar(l[e])) e++;
  if (start) *start = s;
  if (end) *end = e;
  return l.substr(s, e - s);
}

void TextEditor::pushUndo(bool mergeTyping) {
  if (!(mergeTyping && lastWasTyping_)) {
    undo_.push_back({lines_, cursor_});
    if (undo_.size() > 500) undo_.erase(undo_.begin());
  }
  redo_.clear();
  lastWasTyping_ = mergeTyping;
  dirty_ = changed_ = true;
  textVersion_++;
}

// Moves the breakpoints with the text: the lines before and after the changed
// block are matched, so breakpoints below an insertion or deletion move with it.
void TextEditor::syncBreakpoints() {
  if (bpVersion_ == textVersion_) return;
  bpVersion_ = textVersion_;
  if (bps_.empty()) {
    bpBase_.clear();
    return;
  }
  const auto& a = bpBase_;
  const auto& b = lines_;
  size_t n = std::min(a.size(), b.size()), p = 0, q = 0;
  while (p < n && a[p] == b[p]) p++;
  while (q < n - p && a[a.size() - 1 - q] == b[b.size() - 1 - q]) q++;
  long oldEnd = (long)a.size() - (long)q, newEnd = (long)b.size() - (long)q;
  long delta = (long)b.size() - (long)a.size();
  std::set<int> moved;
  for (int l : bps_) {
    if (l < (long)p) moved.insert(l);
    else if (l >= oldEnd) moved.insert((int)(l + delta));
    else if (l < newEnd) moved.insert(l);  // inside the changed block: stays if the line still exists
  }
  if (moved != bps_) bpChanged_ = true;
  bps_ = std::move(moved);
  bpBase_ = lines_;
}

void TextEditor::toggleBreakpoint(int line) {
  if (line < 0 || line >= (int)lines_.size()) return;
  syncBreakpoints();
  if (!bps_.erase(line)) bps_.insert(line);
  bpBase_ = lines_;
  bpVersion_ = textVersion_;
  bpChanged_ = true;
}

void TextEditor::setBreakpoints(const std::set<int>& lines) {
  bps_.clear();
  for (int l : lines)
    if (l >= 0 && l < (int)lines_.size()) bps_.insert(l);
  bpBase_ = lines_;
  bpVersion_ = textVersion_;
}

const std::set<int>& TextEditor::breakpoints() {
  syncBreakpoints();
  return bps_;
}

void TextEditor::undo() {
  if (undo_.empty()) return;
  redo_.push_back({lines_, cursor_});
  lines_ = undo_.back().lines;
  cursor_ = clamp(undo_.back().cursor);
  undo_.pop_back();
  selStart_ = selEnd_ = anchor_ = cursor_;
  lastWasTyping_ = false;
  dirty_ = changed_ = true;
  textVersion_++;
}

void TextEditor::redo() {
  if (redo_.empty()) return;
  undo_.push_back({lines_, cursor_});
  lines_ = redo_.back().lines;
  cursor_ = clamp(redo_.back().cursor);
  redo_.pop_back();
  selStart_ = selEnd_ = anchor_ = cursor_;
  lastWasTyping_ = false;
  dirty_ = changed_ = true;
  textVersion_++;
}

std::string TextEditor::selectedText() const {
  if (!hasSelection()) return {};
  Coord a = selStart_, b = selEnd_;
  if (a.line == b.line) return lines_[a.line].substr(a.col, b.col - a.col);
  std::string s = lines_[a.line].substr(a.col) + "\n";
  for (int i = a.line + 1; i < b.line; i++) s += lines_[i] + "\n";
  s += lines_[b.line].substr(0, b.col);
  return s;
}

void TextEditor::deleteSelection() {
  if (!hasSelection()) return;
  Coord a = selStart_, b = selEnd_;
  lines_[a.line] = lines_[a.line].substr(0, a.col) + lines_[b.line].substr(b.col);
  lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
  cursor_ = selStart_ = selEnd_ = anchor_ = a;
}

void TextEditor::insertText(const std::string& s) {
  deleteSelection();
  std::string& l = lines_[cursor_.line];
  std::string tail = l.substr(cursor_.col);
  l = l.substr(0, cursor_.col);
  size_t pos = 0;
  for (;;) {
    size_t nl = s.find('\n', pos);
    std::string part = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    if (!part.empty() && part.back() == '\r') part.pop_back();
    lines_[cursor_.line] += part;
    if (nl == std::string::npos) break;
    lines_.insert(lines_.begin() + cursor_.line + 1, std::string());
    cursor_.line++;
    pos = nl + 1;
  }
  cursor_.col = (int)lines_[cursor_.line].size();
  lines_[cursor_.line] += tail;
  selStart_ = selEnd_ = anchor_ = cursor_;
  scrollToCursor_ = true;
}

void TextEditor::backspace() {
  if (hasSelection()) {
    pushUndo(false);
    deleteSelection();
    return;
  }
  if (cursor_.col == 0 && cursor_.line == 0) return;
  pushUndo(false);
  if (cursor_.col == 0) {
    int prev = (int)lines_[cursor_.line - 1].size();
    lines_[cursor_.line - 1] += lines_[cursor_.line];
    lines_.erase(lines_.begin() + cursor_.line);
    cursor_ = {cursor_.line - 1, prev};
  } else {
    std::string& l = lines_[cursor_.line];
    int s = cursor_.col - 1;
    while (s > 0 && ((unsigned char)l[s] & 0xC0) == 0x80) s--;
    l.erase(s, cursor_.col - s);
    cursor_.col = s;
  }
  selStart_ = selEnd_ = anchor_ = cursor_;
}

void TextEditor::del() {
  if (hasSelection()) {
    pushUndo(false);
    deleteSelection();
    return;
  }
  std::string& l = lines_[cursor_.line];
  if (cursor_.col >= (int)l.size()) {
    if (cursor_.line + 1 >= (int)lines_.size()) return;
    pushUndo(false);
    l += lines_[cursor_.line + 1];
    lines_.erase(lines_.begin() + cursor_.line + 1);
    return;
  }
  pushUndo(false);
  int e = cursor_.col + 1;
  while (e < (int)l.size() && ((unsigned char)l[e] & 0xC0) == 0x80) e++;
  l.erase(cursor_.col, e - cursor_.col);
}

void TextEditor::newline() {
  pushUndo(false);
  // keep the indentation of the current line
  const std::string& l = lines_[cursor_.line];
  std::string indent;
  for (char c : l) {
    if (c == ' ' || c == '\t') indent += c;
    else break;
  }
  // a label at column 0 still wants the code indented
  if (indent.empty() && !l.empty() && !std::isspace((unsigned char)l[0]) && l[0] != ';') indent = "\t";
  insertText("\n" + indent);
}

void TextEditor::moveCursor(Coord c, bool select) {
  cursor_ = clamp(c);
  if (select) {
    selStart_ = std::min(anchor_, cursor_);
    selEnd_ = std::max(anchor_, cursor_);
  } else {
    anchor_ = selStart_ = selEnd_ = cursor_;
  }
  lastWasTyping_ = false;
  scrollToCursor_ = true;
}

void TextEditor::goTo(int line, int col) {
  moveCursor({line, col}, false);
}

std::pair<int, int> TextEditor::selectedLines() const {
  if (!hasSelection()) return {cursor_.line, cursor_.line};
  int last = selEnd_.line;
  if (selEnd_.col == 0 && last > selStart_.line) last--;  // whole lines selected
  return {selStart_.line, last};
}

void TextEditor::copy() {
  if (hasSelection()) ImGui::SetClipboardText(selectedText().c_str());
  else ImGui::SetClipboardText((lines_[cursor_.line] + "\n").c_str());
}

void TextEditor::cut() {
  copy();
  pushUndo(false);
  if (hasSelection()) deleteSelection();
  else if (lines_.size() > 1) {
    lines_.erase(lines_.begin() + cursor_.line);
    cursor_ = clamp({cursor_.line, 0});
    selStart_ = selEnd_ = anchor_ = cursor_;
  } else lines_[0].clear(), cursor_ = {};
}

void TextEditor::paste() {
  const char* t = ImGui::GetClipboardText();
  if (!t || !*t) return;
  pushUndo(false);
  insertText(t);
}

void TextEditor::selectAll() {
  anchor_ = selStart_ = {0, 0};
  cursor_ = selEnd_ = {(int)lines_.size() - 1, (int)lines_.back().size()};
}

void TextEditor::selectLines(int first, int last) {
  anchor_ = selStart_ = clamp({first, 0});
  cursor_ = selEnd_ = clamp({last + 1, 0});
  if (last + 1 >= (int)lines_.size()) cursor_ = selEnd_ = {(int)lines_.size() - 1, (int)lines_.back().size()};
}

void TextEditor::updateCompletion() {
  completions_.clear();
  completionOpen_ = false;
  if (!hooks_.complete) return;
  int s;
  std::string w = wordAt({cursor_.line, cursor_.col}, &s);
  std::string prefix = lines_[cursor_.line].substr(s, cursor_.col - s);
  if (prefix.size() < 2) return;
  completions_ = hooks_.complete(prefix, lines_[cursor_.line]);
  if (completions_.size() == 1 && completions_[0].word.size() == prefix.size()) completions_.clear();
  completionSel_ = 0;
  completionOpen_ = !completions_.empty();
}

void TextEditor::acceptCompletion() {
  if (!completionOpen_ || completions_.empty()) return;
  int s;
  wordAt(cursor_, &s);
  std::string word = completions_[completionSel_].word;
  // keep the case the user started typing in
  std::string typed = lines_[cursor_.line].substr(s, cursor_.col - s);
  bool lower = !typed.empty() && std::islower((unsigned char)typed[0]);
  if (lower && completions_[completionSel_].kind == TokenKind::Mnemonic)
    for (auto& c : word) c = (char)std::tolower((unsigned char)c);
  pushUndo(false);
  lines_[cursor_.line].replace(s, cursor_.col - s, word);
  cursor_.col = s + (int)word.size();
  selStart_ = selEnd_ = anchor_ = cursor_;
  completionOpen_ = false;
}

void TextEditor::handleKeyboard() {
  ImGuiIO& io = ImGui::GetIO();
  bool shift = io.KeyShift, ctrl = io.KeyCtrl, alt = io.KeyAlt;
  bool wordMove = alt || ctrl;
  auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  int pageLines = std::max(1, (int)(ImGui::GetWindowHeight() / lineH_) - 2);

  if (completionOpen_) {
    if (pressed(ImGuiKey_Escape)) { completionOpen_ = false; return; }
    if (pressed(ImGuiKey_DownArrow)) { completionSel_ = (completionSel_ + 1) % (int)completions_.size(); return; }
    if (pressed(ImGuiKey_UpArrow)) { completionSel_ = (completionSel_ + (int)completions_.size() - 1) % (int)completions_.size(); return; }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_Tab) || pressed(ImGuiKey_KeypadEnter)) { acceptCompletion(); return; }
  }

  if (pressed(ImGuiKey_F12) && hooks_.gotoDefinition) {
    std::string w = wordAt(cursor_);
    if (!w.empty()) hooks_.gotoDefinition(w);
    return;
  }
  if (alt && !ctrl && (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow))) {
    moveLines(pressed(ImGuiKey_UpArrow) ? -1 : 1);
    return;
  }
  if (ctrl && !alt) {
    if (pressed(ImGuiKey_Slash) || pressed(ImGuiKey_KeypadDivide)) { toggleComment(); return; }
    if (pressed(ImGuiKey_D)) { duplicateLines(); return; }
    if (shift && pressed(ImGuiKey_K)) { deleteLines(); return; }
    if (pressed(ImGuiKey_Z)) { shift ? redo() : undo(); return; }
    if (pressed(ImGuiKey_Y)) { redo(); return; }
    if (pressed(ImGuiKey_C)) { copy(); return; }
    if (pressed(ImGuiKey_X)) { cut(); return; }
    if (pressed(ImGuiKey_V)) { paste(); return; }
    if (pressed(ImGuiKey_A)) { selectAll(); return; }
    if (pressed(ImGuiKey_Space)) { updateCompletion(); return; }
  }

  Coord c = cursor_;
  if (pressed(ImGuiKey_LeftArrow)) {
    if (!shift && hasSelection()) { moveCursor(selStart_, false); return; }
    if (wordMove) {
      int s; wordAt(c, &s);
      c.col = s == c.col ? std::max(0, c.col - 1) : s;
      if (c.col == 0 && cursor_.col == 0 && c.line > 0) c = {c.line - 1, (int)lines_[c.line - 1].size()};
    } else if (c.col > 0) {
      c.col--;
      while (c.col > 0 && ((unsigned char)lines_[c.line][c.col] & 0xC0) == 0x80) c.col--;
    } else if (c.line > 0) c = {c.line - 1, (int)lines_[c.line - 1].size()};
    moveCursor(c, shift);
    completionOpen_ = false;
  } else if (pressed(ImGuiKey_RightArrow)) {
    if (!shift && hasSelection()) { moveCursor(selEnd_, false); return; }
    if (wordMove) {
      int e; wordAt(c, nullptr, &e);
      c.col = e == c.col ? c.col + 1 : e;
      if (c.col > (int)lines_[c.line].size() && c.line + 1 < (int)lines_.size()) c = {c.line + 1, 0};
    } else if (c.col < (int)lines_[c.line].size()) {
      c.col++;
      while (c.col < (int)lines_[c.line].size() && ((unsigned char)lines_[c.line][c.col] & 0xC0) == 0x80) c.col++;
    } else if (c.line + 1 < (int)lines_.size()) c = {c.line + 1, 0};
    moveCursor(c, shift);
    completionOpen_ = false;
  } else if (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_PageUp) || pressed(ImGuiKey_PageDown)) {
    int d = pressed(ImGuiKey_UpArrow) ? -1 : pressed(ImGuiKey_DownArrow) ? 1 : pressed(ImGuiKey_PageUp) ? -pageLines : pageLines;
    int v = visualCol(c.line, c.col);
    c.line = std::clamp(c.line + d, 0, (int)lines_.size() - 1);
    c.col = byteCol(c.line, v);
    moveCursor(c, shift);
  } else if (pressed(ImGuiKey_Home)) {
    int first = 0;
    while (first < (int)lines_[c.line].size() && std::isspace((unsigned char)lines_[c.line][first])) first++;
    c.col = c.col == first ? 0 : first;
    if (ctrl) c = {0, 0};
    moveCursor(c, shift);
  } else if (pressed(ImGuiKey_End)) {
    c.col = (int)lines_[c.line].size();
    if (ctrl) c = {(int)lines_.size() - 1, (int)lines_.back().size()};
    moveCursor(c, shift);
  } else if (pressed(ImGuiKey_Backspace)) {
    backspace();
    if (completionOpen_) updateCompletion();
  } else if (pressed(ImGuiKey_Delete)) {
    del();
  } else if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
    newline();
  } else if (pressed(ImGuiKey_Tab)) {
    if (hasSelection() && selStart_.line != selEnd_.line) {
      pushUndo(false);
      auto [a, b] = selectedLines();
      for (int i = a; i <= b; i++) {
        if (shift) {
          if (!lines_[i].empty() && lines_[i][0] == '\t') lines_[i].erase(0, 1);
        } else lines_[i].insert(0, "\t");
      }
      selStart_ = {a, 0};
      selEnd_ = clamp({b, (int)lines_[b].size()});
    } else if (!shift) {
      pushUndo(false);
      insertText("\t");
    }
  } else if (pressed(ImGuiKey_Escape)) {
    completionOpen_ = false;
  }

  // typed characters
  if (!ctrl && io.InputQueueCharacters.Size > 0) {
    std::string s;
    for (ImWchar w : io.InputQueueCharacters) {
      unsigned int cp = w;
      if (cp < 32 || cp == 127) continue;
      if (cp < 0x80) s += (char)cp;
      else if (cp < 0x800) s += (char)(0xC0 | (cp >> 6)), s += (char)(0x80 | (cp & 0x3F));
      else s += (char)(0xE0 | (cp >> 12)), s += (char)(0x80 | ((cp >> 6) & 0x3F)), s += (char)(0x80 | (cp & 0x3F));
    }
    io.InputQueueCharacters.resize(0);
    if (!s.empty()) {
      pushUndo(s.size() == 1 && s[0] != ' ');
      insertText(s);
      if (isWordChar(s.back())) updateCompletion();
      else completionOpen_ = false;
    }
  }
}

// ---------------------------------------------------------------------------
// Search

static char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

std::vector<Match> findMatches(const std::vector<std::string>& lines, const std::string& query, FindOptions opt) {
  std::vector<Match> out;
  if (query.empty()) return out;
  std::string q = query;
  if (!opt.matchCase) for (auto& c : q) c = lowerAscii(c);
  for (int i = 0; i < (int)lines.size(); i++) {
    std::string l = lines[i];
    if (!opt.matchCase) for (auto& c : l) c = lowerAscii(c);
    for (size_t pos = l.find(q); pos != std::string::npos; pos = l.find(q, pos + 1)) {
      size_t end = pos + q.size();
      if (opt.wholeWord && ((pos > 0 && isWordChar(l[pos - 1])) || (end < l.size() && isWordChar(l[end])))) continue;
      out.push_back({i, (int)pos, (int)end});
      pos = end - 1;  // no overlapping matches
    }
  }
  return out;
}

void TextEditor::refreshMatches() {
  if (bar_ == Bar::None || bar_ == Bar::Goto || findText_.empty()) {
    matches_.clear();
    currentMatch_ = -1;
    return;
  }
  if (matchesVersion_ == textVersion_ && matchesQuery_ == findText_ && matchesOpt_.matchCase == findOpt_.matchCase &&
      matchesOpt_.wholeWord == findOpt_.wholeWord)
    return;
  matches_ = findMatches(lines_, findText_, findOpt_);
  matchesVersion_ = textVersion_;
  matchesQuery_ = findText_;
  matchesOpt_ = findOpt_;
  currentMatch_ = -1;
  for (int i = 0; i < (int)matches_.size(); i++)
    if (hasSelection() && matches_[i].line == selStart_.line && matches_[i].start == selStart_.col) currentMatch_ = i;
}

void TextEditor::setSelection(Coord a, Coord b) {
  anchor_ = selStart_ = clamp(a);
  cursor_ = selEnd_ = clamp(b);
  scrollToCursor_ = true;
  lastWasTyping_ = false;
}

std::string TextEditor::wordAtCursor() const { return wordAt(cursor_); }

void TextEditor::openFind(bool replace) {
  bar_ = replace ? Bar::Replace : Bar::Find;
  focusBar_ = true;
  if (hasSelection() && selStart_.line == selEnd_.line) findText_ = selectedText();
  else if (findText_.empty()) findText_ = wordAt(cursor_);
  matchesVersion_ = 0;
}

void TextEditor::openGoto() {
  bar_ = Bar::Goto;
  focusBar_ = true;
  gotoText_ = std::to_string(cursor_.line + 1);
}

void TextEditor::findNext(bool backwards) {
  refreshMatches();
  if (matches_.empty()) return;
  Coord from = backwards ? selStart_ : (hasSelection() ? selEnd_ : cursor_);
  int pick = -1;
  if (!backwards) {
    for (int i = 0; i < (int)matches_.size(); i++)
      if (Coord{matches_[i].line, matches_[i].start} != selStart_ || !hasSelection())
        if (!(Coord{matches_[i].line, matches_[i].start} < from)) { pick = i; break; }
    if (pick < 0) pick = 0;  // wrap around
  } else {
    for (int i = (int)matches_.size() - 1; i >= 0; i--)
      if (Coord{matches_[i].line, matches_[i].start} < from) { pick = i; break; }
    if (pick < 0) pick = (int)matches_.size() - 1;
  }
  currentMatch_ = pick;
  setSelection({matches_[pick].line, matches_[pick].start}, {matches_[pick].line, matches_[pick].end});
  centerOnCursor_ = true;
}

bool TextEditor::replaceCurrent() {
  refreshMatches();
  for (auto& m : matches_)
    if (hasSelection() && selStart_ == Coord{m.line, m.start} && selEnd_ == Coord{m.line, m.end}) {
      pushUndo(false);
      lines_[m.line].replace(m.start, m.end - m.start, replaceText_);
      setSelection({m.line, m.start}, {m.line, m.start + (int)replaceText_.size()});
      refreshMatches();
      findNext();
      return true;
    }
  findNext();
  return false;
}

int TextEditor::replaceAll() {
  refreshMatches();
  if (matches_.empty()) return 0;
  pushUndo(false);
  int n = (int)matches_.size();
  for (int i = n - 1; i >= 0; i--) {
    auto& m = matches_[i];
    lines_[m.line].replace(m.start, m.end - m.start, replaceText_);
  }
  cursor_ = clamp(cursor_);
  selStart_ = selEnd_ = anchor_ = cursor_;
  refreshMatches();
  return n;
}

// ---------------------------------------------------------------------------
// Line editing

void TextEditor::toggleComment() {
  auto [a, b] = selectedLines();
  const std::string& p = commentPrefix_;
  bool all = true, any = false;
  for (int i = a; i <= b; i++) {
    size_t f = lines_[i].find_first_not_of(" \t");
    if (f == std::string::npos) continue;
    any = true;
    if (lines_[i].compare(f, p.size(), p) != 0) all = false;
  }
  if (!any) return;
  pushUndo(false);
  for (int i = a; i <= b; i++) {
    std::string& l = lines_[i];
    size_t f = l.find_first_not_of(" \t");
    if (f == std::string::npos) continue;
    if (all) {
      size_t len = p.size() + (l.compare(f + p.size(), 1, " ") == 0 && p != ";" ? 1 : 0);
      l.erase(f, len);
    } else {
      // DASM comments go at column 0; "rem" and "//" after the indentation
      // (in batari Basic a word at column 0 would be a label).
      if (p == ";") l.insert(0, p);
      else l.insert(f, p + " ");
    }
  }
  if (hasSelection()) setSelection({a, 0}, {b, (int)lines_[b].size()});
  else cursor_ = clamp(cursor_), selStart_ = selEnd_ = anchor_ = cursor_;
}

void TextEditor::duplicateLines() {
  auto [a, b] = selectedLines();
  pushUndo(false);
  std::vector<std::string> block(lines_.begin() + a, lines_.begin() + b + 1);
  lines_.insert(lines_.begin() + b + 1, block.begin(), block.end());
  int n = b - a + 1;
  if (hasSelection()) setSelection({selStart_.line + n, selStart_.col}, {selEnd_.line + n, selEnd_.col});
  else moveCursor({cursor_.line + n, cursor_.col}, false);
}

void TextEditor::moveLines(int delta) {
  auto [a, b] = selectedLines();
  if ((delta < 0 && a == 0) || (delta > 0 && b + 1 >= (int)lines_.size())) return;
  pushUndo(false);
  if (delta < 0) {
    std::string above = lines_[a - 1];
    lines_.erase(lines_.begin() + a - 1);
    lines_.insert(lines_.begin() + b, above);
  } else {
    std::string below = lines_[b + 1];
    lines_.erase(lines_.begin() + b + 1);
    lines_.insert(lines_.begin() + a, below);
  }
  Coord s = selStart_, e = selEnd_, c = cursor_;
  s.line += delta, e.line += delta, c.line += delta;
  if (hasSelection()) setSelection(s, e);
  else moveCursor(c, false);
}

void TextEditor::deleteLines() {
  auto [a, b] = selectedLines();
  pushUndo(false);
  lines_.erase(lines_.begin() + a, lines_.begin() + b + 1);
  if (lines_.empty()) lines_.push_back("");
  moveCursor({std::min(a, (int)lines_.size() - 1), 0}, false);
}

void TextEditor::renderFindBar() {
  ImGuiIO& io = ImGui::GetIO();
  if (bar_ == Bar::None) return;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
  ImGui::PushID("findbar");
  bool close = false;
  if (bar_ == Bar::Goto) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Go to line"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (focusBar_) ImGui::SetKeyboardFocusHere(), focusBar_ = false;
    if (ImGui::InputText("##goto", &gotoText_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal)) {
      int n = std::atoi(gotoText_.c_str());
      if (n > 0) goTo(n - 1), centerOnCursor_ = true;
      close = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("1-%d", (int)lines_.size());
  } else {
    ImGui::SetNextItemWidth(280);
    if (focusBar_) ImGui::SetKeyboardFocusHere(), focusBar_ = false;
    bool enter = ImGui::InputTextWithHint("##find", T("Find"), &findText_, ImGuiInputTextFlags_EnterReturnsTrue);
    bool findActive = ImGui::IsItemActive() || enter;
    if (enter) {
      findNext(io.KeyShift);
      ImGui::SetKeyboardFocusHere(-1);  // keep typing in the box after Enter
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Aa"), &findOpt_.matchCase)) matchesVersion_ = 0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Match case"));
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Word"), &findOpt_.wholeWord)) matchesVersion_ = 0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Whole word"));
    ImGui::SameLine();
    refreshMatches();
    if (findText_.empty()) ImGui::TextDisabled(" ");
    else if (matches_.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", T("No results"));
    else ImGui::TextDisabled("%s", F("{0} of {1}", {currentMatch_ < 0 ? "?" : std::to_string(currentMatch_ + 1),
                                                       std::to_string(matches_.size())}).c_str());
    ImGui::SameLine();
    if (ImGui::ArrowButton("##prev", ImGuiDir_Up)) findNext(true);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Down)) findNext(false);
    if (bar_ == Bar::Replace) {
      ImGui::SetNextItemWidth(280);
      bool rep = ImGui::InputTextWithHint("##replace", T("Replace"), &replaceText_, ImGuiInputTextFlags_EnterReturnsTrue);
      if (rep) {
        replaceCurrent();
        ImGui::SetKeyboardFocusHere(-1);
      }
      ImGui::SameLine();
      if (ImGui::Button(T("Replace"))) replaceCurrent();
      ImGui::SameLine();
      if (ImGui::Button(T("Replace all"))) replaceAll();
    }
    (void)findActive;
  }
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight());
  if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) close = true;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) close = true;
  if (close) {
    bar_ = Bar::None;
    matches_.clear();
    focusEditor_ = true;
  }
  ImGui::PopID();
  ImGui::PopStyleVar();
  ImGui::Separator();
}

bool TextEditor::render(const char* id, ImFont* font) {
  changed_ = false;
  ImGui::PushFont(font);
  charW_ = ImGui::CalcTextSize("M").x;
  lineH_ = ImGui::GetTextLineHeight() + 2;
  int numChars = std::max(4, (int)std::to_string(lines_.size()).size() + 1);
  float numW = numChars * charW_;
  float cycW = gutterChars_ * charW_;
  float bpW = charW_ * 1.8f;  // breakpoint column
  float gutterW = bpW + numW + cycW + charW_ * 1.5f;
  syncBreakpoints();

  int maxCols = 0;
  for (int i = 0; i < (int)lines_.size(); i++) maxCols = std::max(maxCols, visualCol(i, (int)lines_[i].size()));

  // Editor-wide shortcuts (work from the text and from the find bar).
  {
    ImGuiIO& kio = ImGui::GetIO();
    bool here = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (here && kio.KeyCtrl && !kio.KeyAlt) {
      if (ImGui::IsKeyPressed(ImGuiKey_F, false)) openFind(false);
      if (ImGui::IsKeyPressed(ImGuiKey_H, false) || (kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))) openFind(true);
      if (!kio.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G, false)) openGoto();  // Ctrl+Shift+G: go to address (app)
    }
    if (here && ImGui::IsKeyPressed(ImGuiKey_F9, false)) toggleBreakpoint(cursor_.line);
    if (here && ImGui::IsKeyPressed(ImGuiKey_F3)) {
      if (bar_ == Bar::None) openFind(false), focusBar_ = false;
      findNext(kio.KeyShift);
    }
  }
  renderFindBar();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(30, 30, 30, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (focusEditor_) {
    ImGui::SetNextWindowFocus();
    focusEditor_ = false;
  }
  ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs);
  ImGui::PopStyleVar();

  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 contentSize(gutterW + (maxCols + 4) * charW_, lines_.size() * lineH_ + ImGui::GetWindowHeight() * 0.5f);
  ImGui::InvisibleButton("##text", ImVec2(std::max(contentSize.x, ImGui::GetContentRegionAvail().x), contentSize.y));
  bool hovered = ImGui::IsItemHovered();
  bool focused = ImGui::IsWindowFocused();
  ImGui::SetItemKeyOwner(ImGuiKey_Tab);
  ImGui::SetItemKeyOwner(ImGuiKey_UpArrow);
  ImGui::SetItemKeyOwner(ImGuiKey_DownArrow);

  ImGuiIO& io = ImGui::GetIO();
  float scrollX = ImGui::GetScrollX();
  float textX = origin.x + gutterW;

  auto mouseCoord = [&]() {
    ImVec2 m = io.MousePos;
    int line = std::clamp((int)std::floor((m.y - origin.y) / lineH_), 0, (int)lines_.size() - 1);
    int vis = std::max(0, (int)std::lround((m.x - textX) / charW_));
    return clamp({line, byteCol(line, vis)});
  };

  // mouse
  bool inMargin = io.MousePos.x < ImGui::GetWindowPos().x + bpW + numW;
  if (hovered && inMargin && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    toggleBreakpoint(std::clamp((int)std::floor((io.MousePos.y - origin.y) / lineH_), 0, (int)lines_.size() - 1));
  } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyCtrl && hooks_.gotoDefinition) {
    Coord c = mouseCoord();
    moveCursor(c, false);
    std::string w = wordAt(c);
    if (!w.empty()) hooks_.gotoDefinition(w);
  } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    Coord c = mouseCoord();
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      int s, e;
      wordAt(c, &s, &e);
      anchor_ = selStart_ = {c.line, s};
      cursor_ = selEnd_ = {c.line, e};
    } else {
      if (!io.KeyShift) anchor_ = c;
      moveCursor(c, io.KeyShift);
    }
    completionOpen_ = false;
  } else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    moveCursor(mouseCoord(), true);
    scrollToCursor_ = true;
  }

  if (focused) handleKeyboard();

  // visible lines
  float scrollY = ImGui::GetScrollY();
  float viewH = ImGui::GetWindowHeight();
  int first = std::max(0, (int)(scrollY / lineH_) - 1);
  int last = std::min((int)lines_.size() - 1, (int)((scrollY + viewH) / lineH_) + 1);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float fontSize = ImGui::GetFontSize();
  ImVec2 winPos = ImGui::GetWindowPos();

  for (int i = first; i <= last; i++) {
    float y = origin.y + i * lineH_;
    // current line
    if (i == cursor_.line && !hasSelection())
      dl->AddRectFilled(ImVec2(winPos.x, y), ImVec2(winPos.x + ImGui::GetWindowWidth() + scrollX, y + lineH_), IM_COL32(40, 40, 46, 255));
    // marker background
    auto mk = markers_.find(i + 1);
    if (mk != markers_.end())
      dl->AddRectFilled(ImVec2(textX, y), ImVec2(textX + contentSize.x, y + lineH_),
                        mk->second.first ? IM_COL32(110, 30, 30, 120) : IM_COL32(110, 90, 20, 110));
    // search matches
    for (int k = 0; k < (int)matches_.size(); k++) {
      const Match& m = matches_[k];
      if (m.line != i) continue;
      float x0 = textX + visualCol(i, m.start) * charW_, x1 = textX + visualCol(i, m.end) * charW_;
      dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + lineH_), k == currentMatch_ ? IM_COL32(200, 140, 30, 150) : IM_COL32(120, 100, 40, 110));
    }
    // selection
    if (hasSelection() && i >= selStart_.line && i <= selEnd_.line) {
      int a = i == selStart_.line ? visualCol(i, selStart_.col) : 0;
      int b = i == selEnd_.line ? visualCol(i, selEnd_.col) : visualCol(i, (int)lines_[i].size()) + 1;
      dl->AddRectFilled(ImVec2(textX + a * charW_, y), ImVec2(textX + b * charW_, y + lineH_), IM_COL32(38, 79, 120, 255));
    }
    // text
    const std::string& l = lines_[i];
    std::vector<Token> toks = hooks_.tokenize ? hooks_.tokenize(l) : std::vector<Token>{};
    size_t ti = 0;
    int v = 0;
    for (int b = 0; b < (int)l.size();) {
      while (ti < toks.size() && toks[ti].end <= b) ti++;
      TokenKind k = (ti < toks.size() && toks[ti].start <= b) ? toks[ti].kind : TokenKind::Text;
      unsigned char c = l[b];
      if (c == '\t') {
        v = (v / tabSize_ + 1) * tabSize_;
        b++;
        continue;
      }
      int len = 1;
      if (c >= 0xF0) len = 4; else if (c >= 0xE0) len = 3; else if (c >= 0xC0) len = 2;
      dl->AddText(font, fontSize, ImVec2(textX + v * charW_, y + 1), colorOf(k), l.data() + b, l.data() + std::min((int)l.size(), b + len));
      v++;
      b += len;
    }
  }

  // gutter (drawn over the text so it stays fixed when scrolling horizontally)
  float gx = winPos.x;
  dl->AddRectFilled(ImVec2(gx, winPos.y), ImVec2(gx + gutterW - charW_ * 0.5f, winPos.y + viewH), IM_COL32(30, 30, 30, 255));
  for (int i = first; i <= last; i++) {
    float y = origin.y + i * lineH_;
    char num[32];
    snprintf(num, sizeof num, "%*d", std::min(numChars - 1, 12), i + 1);
    auto mk = markers_.find(i + 1);
    ImU32 numCol = mk != markers_.end() ? (mk->second.first ? IM_COL32(240, 90, 90, 255) : IM_COL32(230, 190, 80, 255))
                   : i == cursor_.line ? IM_COL32(200, 200, 200, 255) : IM_COL32(110, 110, 110, 255);
    if (bps_.count(i))
      dl->AddCircleFilled(ImVec2(gx + bpW * 0.5f, y + lineH_ * 0.5f), std::min(bpW, lineH_) * 0.32f, IM_COL32(220, 60, 60, 255));
    dl->AddText(font, fontSize, ImVec2(gx + bpW, y + 1), numCol, num);
    if (cycW > 0 && hooks_.gutter) {
      std::string g = hooks_.gutter(i, lines_[i]);
      if (!g.empty()) {
        float w = ImGui::CalcTextSize(g.c_str()).x;
        dl->AddText(font, fontSize, ImVec2(gx + bpW + numW + cycW - w, y + 1), IM_COL32(150, 150, 90, 255), g.c_str());
      }
    }
  }
  dl->AddLine(ImVec2(gx + gutterW - charW_ * 0.5f, winPos.y), ImVec2(gx + gutterW - charW_ * 0.5f, winPos.y + viewH), IM_COL32(60, 60, 60, 255));
  // offset the gutter by the horizontal scroll so it stays on screen
  (void)scrollX;

  // cursor
  if (focused) {
    float cx = textX + visualCol(cursor_.line, cursor_.col) * charW_;
    float cy = origin.y + cursor_.line * lineH_;
    if (std::fmod(ImGui::GetTime(), 1.0) < 0.6 || io.InputQueueCharacters.Size)
      dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + 2, cy + lineH_), IM_COL32(230, 230, 230, 255));
    // completion popup
    if (completionOpen_) {
      ImGui::SetNextWindowPos(ImVec2(cx, cy + lineH_));
      ImGui::SetNextWindowBgAlpha(0.97f);
      ImGui::BeginTooltip();
      for (int k = 0; k < (int)completions_.size() && k < 12; k++) {
        bool sel = k == completionSel_;
        ImGui::PushStyleColor(ImGuiCol_Text, colorOf(completions_[k].kind));
        ImGui::Selectable(completions_[k].word.c_str(), sel, 0, ImVec2(charW_ * 10, 0));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", completions_[k].detail.c_str());
      }
      ImGui::EndTooltip();
    }
  }

  // hover help
  if (hovered && hooks_.hover && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !completionOpen_ &&
      ImGui::GetIO().MouseDelta.x == 0 && ImGui::GetIO().MouseDelta.y == 0) {
    ImVec2 m = io.MousePos;
    int line = (int)std::floor((m.y - origin.y) / lineH_);
    if (line >= 0 && line < (int)lines_.size() && m.x > textX) {
      Coord c = clamp({line, byteCol(line, (int)((m.x - textX) / charW_))});
      std::string w = wordAt(c);
      auto mk = markers_.find(line + 1);
      std::string help = w.empty() ? std::string() : hooks_.hover(w, lines_[line]);
      if (mk != markers_.end()) help = mk->second.second + (help.empty() ? "" : "\n\n" + help);
      if (!help.empty() && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(charW_ * 70);
        ImGui::TextUnformatted(help.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
    }
  }

  if (scrollToCursor_) {
    float cy = cursor_.line * lineH_;
    if (centerOnCursor_ && (cy < scrollY + lineH_ * 2 || cy > scrollY + viewH - lineH_ * 3))
      ImGui::SetScrollY(std::max(0.0f, cy - viewH * 0.4f));
    else if (cy < scrollY) ImGui::SetScrollY(cy);
    else if (cy + lineH_ * 2 > scrollY + viewH) ImGui::SetScrollY(cy + lineH_ * 2 - viewH);
    float cx = gutterW + visualCol(cursor_.line, cursor_.col) * charW_;
    float vw = ImGui::GetWindowWidth();
    if (cx - gutterW < scrollX) ImGui::SetScrollX(std::max(0.0f, cx - gutterW - charW_ * 4));
    else if (cx > scrollX + vw - charW_ * 2) ImGui::SetScrollX(cx - vw + charW_ * 4);
    scrollToCursor_ = false;
    centerOnCursor_ = false;
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopFont();
  return changed_;
}

}  // namespace vcs
