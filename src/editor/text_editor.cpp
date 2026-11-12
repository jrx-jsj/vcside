// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "text_editor.h"

#include "imgui_stdlib.h"
#include "i18n.h"
#include "theme_ui.h"
#include "../source_links.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace vcs {

using theme::Role;
using theme::col;

static ImU32 colorOf(TokenKind kind) {
  switch (kind) {
    case TokenKind::Comment: return col(Role::SynComment);
    case TokenKind::String: return col(Role::SynString);
    case TokenKind::Number: return col(Role::SynNumber);
    case TokenKind::Mnemonic: return col(Role::SynMnemonic);
    case TokenKind::IllegalMnemonic: return col(Role::SynIllegal);
    case TokenKind::UnstableMnemonic: return col(Role::SynUnstable);
    case TokenKind::Register: return col(Role::SynRegister);
    case TokenKind::Directive: return col(Role::SynDirective);
    case TokenKind::Label: return col(Role::SynLabel);
    case TokenKind::Symbol: return col(Role::SynSymbol);
    default: return col(Role::EdText);
  }
}

static bool isWordChar(char character) { return std::isalnum((unsigned char)character) || character == '_' || character == '.' || (unsigned char)character >= 0x80; }

TextEditor::TextEditor() {}

void TextEditor::setText(const std::string& text) {
  lines_.clear();
  std::string currentLine;
  for (char character : text) {
    if (character == '\n') {
      if (!currentLine.empty() && currentLine.back() == '\r') currentLine.pop_back();
      lines_.push_back(currentLine);
      currentLine.clear();
    } else currentLine += character;
  }
  if (!currentLine.empty() && currentLine.back() == '\r') currentLine.pop_back();
  lines_.push_back(currentLine);
  cursor_ = selStart_ = selEnd_ = anchor_ = {};
  undo_.clear();
  redo_.clear();
  dirty_ = false;
}

void TextEditor::replaceText(const std::string& text) {
  if (text == this->text()) return;
  pushUndo(false);
  auto undo = std::move(undo_);
  Coord keep = cursor_;
  setText(text);  // clears the history: it is put back
  undo_ = std::move(undo);
  cursor_ = selStart_ = selEnd_ = anchor_ = clamp(keep);
  dirty_ = changed_ = true;
  textVersion_++;
}

std::string TextEditor::text() const {
  std::string result;
  for (size_t i = 0; i < lines_.size(); i++) {
    result += lines_[i];
    if (i + 1 < lines_.size()) result += '\n';
  }
  return result;
}

Coord TextEditor::clamp(Coord coord) const {
  coord.line = std::clamp(coord.line, 0, (int)lines_.size() - 1);
  coord.col = std::clamp(coord.col, 0, (int)lines_[coord.line].size());
  // never stop inside a UTF-8 sequence
  while (coord.col > 0 && coord.col < (int)lines_[coord.line].size() && ((unsigned char)lines_[coord.line][coord.col] & 0xC0) == 0x80) coord.col--;
  return coord;
}

int TextEditor::visualCol(int line, int byteCol) const {
  const std::string& lineText = lines_[line];
  int visualColumn = 0;
  for (int i = 0; i < byteCol && i < (int)lineText.size(); i++) {
    unsigned char character = lineText[i];
    if (character == '\t') visualColumn = (visualColumn / tabSize_ + 1) * tabSize_;
    else if ((character & 0xC0) != 0x80) visualColumn++;
  }
  return visualColumn;
}

int TextEditor::byteCol(int line, int visCol) const {
  const std::string& lineText = lines_[line];
  int visualColumn = 0, i = 0;
  while (i < (int)lineText.size()) {
    unsigned char character = lineText[i];
    int charWidth = character == '\t' ? (visualColumn / tabSize_ + 1) * tabSize_ - visualColumn : 1;
    if (visualColumn + charWidth / 2.0f >= visCol + 0.5f && visualColumn + charWidth > visCol) {
      if (visCol - visualColumn < (charWidth + 1) / 2) break;
    }
    if (visualColumn >= visCol) break;
    visualColumn += charWidth;
    i++;
    while (i < (int)lineText.size() && ((unsigned char)lineText[i] & 0xC0) == 0x80) i++;
  }
  return i;
}

std::string TextEditor::wordAt(Coord coord, int* start, int* end) const {
  const std::string& lineText = lines_[coord.line];
  int wordStart = coord.col, wordEnd = coord.col;
  while (wordStart > 0 && isWordChar(lineText[wordStart - 1])) wordStart--;
  while (wordEnd < (int)lineText.size() && isWordChar(lineText[wordEnd])) wordEnd++;
  if (start) *start = wordStart;
  if (end) *end = wordEnd;
  return lineText.substr(wordStart, wordEnd - wordStart);
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
// Lines of an older text, moved to where they are in the new one: lines above the change
// stay, lines below it shift, lines inside it stay if the text still has them.
static std::set<int> followEdit(const std::set<int>& marks, const std::vector<std::string>& oldLines, const std::vector<std::string>& newLines) {
  size_t commonSize = std::min(oldLines.size(), newLines.size()), prefixLength = 0, suffixLength = 0;
  while (prefixLength < commonSize && oldLines[prefixLength] == newLines[prefixLength]) prefixLength++;
  while (suffixLength < commonSize - prefixLength && oldLines[oldLines.size() - 1 - suffixLength] == newLines[newLines.size() - 1 - suffixLength]) suffixLength++;
  long oldEnd = (long)oldLines.size() - (long)suffixLength, newEnd = (long)newLines.size() - (long)suffixLength;
  long delta = (long)newLines.size() - (long)oldLines.size();
  std::set<int> moved;
  for (int line : marks) {
    if (line < (long)prefixLength) moved.insert(line);
    else if (line >= oldEnd) moved.insert((int)(line + delta));
    else if (line < newEnd) moved.insert(line);
  }
  return moved;
}

void TextEditor::syncBreakpoints() {
  if (bpVersion_ == textVersion_) return;
  bpVersion_ = textVersion_;
  if (bps_.empty()) {
    bpBase_.clear();
    return;
  }
  std::set<int> moved = followEdit(bps_, bpBase_, lines_);
  if (moved != bps_) bpChanged_ = true;
  bps_ = std::move(moved);
  bpBase_ = lines_;
}

void TextEditor::syncFolds() {
  if (regionsVersion_ != textVersion_) {
    regionsVersion_ = textVersion_;
    regions_ = foldRegions(lines_, foldSyntax_);
    rowsDirty_ = true;
  }
  if (foldVersion_ != textVersion_) {
    foldVersion_ = textVersion_;
    if (!folds_.empty()) {
      std::set<int> moved = followEdit(folds_, foldBase_, lines_), kept;
      for (int line : moved)
        if (regionAt(line)) kept.insert(line);  // the region is still there
      folds_ = std::move(kept);
    }
    foldBase_ = lines_;
    rowsDirty_ = true;
  }
}

const FoldRegion* TextEditor::regionAt(int first) const {
  auto found = std::lower_bound(regions_.begin(), regions_.end(), first, [](const FoldRegion& region, int line) { return region.first < line; });
  return found != regions_.end() && found->first == first ? &*found : nullptr;
}

void TextEditor::updateRows() {
  syncFolds();
  if (!rowsDirty_ && rowOf_.size() == lines_.size()) return;
  rowsDirty_ = false;
  int lineCount = (int)lines_.size();
  std::vector<char> hidden(lineCount, 0);
  for (int foldStart : folds_)
    if (const FoldRegion* region = regionAt(foldStart))
      for (int line = region->first + 1; line <= region->last && line < lineCount; line++) hidden[line] = 1;
  rowOf_.assign(lineCount, 0);
  lineAt_.clear();
  for (int line = 0; line < lineCount; line++) {
    if (!hidden[line]) lineAt_.push_back(line);
    rowOf_[line] = std::max(0, (int)lineAt_.size() - 1);
  }
  if (lineAt_.empty()) lineAt_.push_back(0);
}

bool TextEditor::foldAt(int line) {
  syncFolds();
  const FoldRegion* best = regionAt(line);
  if (!best || folds_.count(line))  // the innermost region around it that is still open
    for (auto& region : regions_)
      if (region.first < line && region.last >= line && !folds_.count(region.first) && (!best || folds_.count(best->first) || region.first > best->first)) best = &region;
  if (!best || folds_.count(best->first)) return false;
  folds_.insert(best->first);
  rowsDirty_ = true;
  // the cursor goes to the line that stays
  if (cursor_.line > best->first && cursor_.line <= best->last) moveCursor({best->first, (int)lines_[best->first].size()}, false);
  return true;
}

bool TextEditor::unfoldAt(int line) {
  syncFolds();
  if (folds_.erase(line)) return rowsDirty_ = true;
  int best = -1;
  for (int foldStart : folds_)
    if (const FoldRegion* region = regionAt(foldStart); region && region->first < line && region->last >= line) best = std::max(best, foldStart);
  if (best < 0) return false;
  folds_.erase(best);
  return rowsDirty_ = true;
}

void TextEditor::foldAll() {
  syncFolds();
  for (auto& region : regions_) folds_.insert(region.first);
  rowsDirty_ = true;
  updateRows();
  if (hiddenLine(cursor_.line)) moveCursor({lineAt_[rowOf_[cursor_.line]], 0}, false);
}

void TextEditor::unfoldAll() {
  folds_.clear();
  rowsDirty_ = true;
}

bool TextEditor::folded(int line) {
  syncFolds();
  return folds_.count(line) > 0;
}

bool TextEditor::hiddenLine(int line) {
  updateRows();
  return line >= 0 && line < (int)rowOf_.size() && lineAt_[rowOf_[line]] != line;
}

void TextEditor::reveal(int line) {
  updateRows();
  while (hiddenLine(line)) {
    int open = lineAt_[rowOf_[line]];  // the fold that hides it starts on its row
    if (!folds_.erase(open)) break;
    rowsDirty_ = true;
  }
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
  for (int line : lines)
    if (line >= 0 && line < (int)lines_.size()) bps_.insert(line);
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
  Coord selectionStart = selStart_, selectionEnd = selEnd_;
  if (selectionStart.line == selectionEnd.line) return lines_[selectionStart.line].substr(selectionStart.col, selectionEnd.col - selectionStart.col);
  std::string selected = lines_[selectionStart.line].substr(selectionStart.col) + "\n";
  for (int i = selectionStart.line + 1; i < selectionEnd.line; i++) selected += lines_[i] + "\n";
  selected += lines_[selectionEnd.line].substr(0, selectionEnd.col);
  return selected;
}

void TextEditor::deleteSelection() {
  if (!hasSelection()) return;
  Coord selectionStart = selStart_, selectionEnd = selEnd_;
  lines_[selectionStart.line] = lines_[selectionStart.line].substr(0, selectionStart.col) + lines_[selectionEnd.line].substr(selectionEnd.col);
  lines_.erase(lines_.begin() + selectionStart.line + 1, lines_.begin() + selectionEnd.line + 1);
  cursor_ = selStart_ = selEnd_ = anchor_ = selectionStart;
}

void TextEditor::insertText(const std::string& inserted) {
  deleteSelection();
  std::string& currentLine = lines_[cursor_.line];
  std::string tail = currentLine.substr(cursor_.col);
  currentLine = currentLine.substr(0, cursor_.col);
  size_t pos = 0;
  for (;;) {
    size_t newlineIndex = inserted.find('\n', pos);
    std::string part = inserted.substr(pos, newlineIndex == std::string::npos ? std::string::npos : newlineIndex - pos);
    if (!part.empty() && part.back() == '\r') part.pop_back();
    lines_[cursor_.line] += part;
    if (newlineIndex == std::string::npos) break;
    lines_.insert(lines_.begin() + cursor_.line + 1, std::string());
    cursor_.line++;
    pos = newlineIndex + 1;
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
  // in an indentation of spaces, back to the level before
  IndentStyle style = cursor_.col > 0 ? indentStyle() : IndentStyle{};
  std::string& current = lines_[cursor_.line];
  if (!style.tabs && cursor_.col > 1 && leadingWhitespace(current).size() >= (size_t)cursor_.col &&
      current.compare(0, cursor_.col, std::string(cursor_.col, ' ')) == 0) {
    int level = (cursor_.col - 1) / style.width * style.width;
    current.erase(level, cursor_.col - level);
    cursor_.col = level;
    selStart_ = selEnd_ = anchor_ = cursor_;
    return;
  }
  if (cursor_.col == 0) {
    int previousSize = (int)lines_[cursor_.line - 1].size();
    lines_[cursor_.line - 1] += lines_[cursor_.line];
    lines_.erase(lines_.begin() + cursor_.line);
    cursor_ = {cursor_.line - 1, previousSize};
  } else {
    std::string& lineText = lines_[cursor_.line];
    int charStart = cursor_.col - 1;
    while (charStart > 0 && ((unsigned char)lineText[charStart] & 0xC0) == 0x80) charStart--;
    lineText.erase(charStart, cursor_.col - charStart);
    cursor_.col = charStart;
  }
  selStart_ = selEnd_ = anchor_ = cursor_;
}

void TextEditor::del() {
  if (hasSelection()) {
    pushUndo(false);
    deleteSelection();
    return;
  }
  std::string& lineText = lines_[cursor_.line];
  if (cursor_.col >= (int)lineText.size()) {
    if (cursor_.line + 1 >= (int)lines_.size()) return;
    pushUndo(false);
    lineText += lines_[cursor_.line + 1];
    lines_.erase(lines_.begin() + cursor_.line + 1);
    return;
  }
  pushUndo(false);
  int charEnd = cursor_.col + 1;
  while (charEnd < (int)lineText.size() && ((unsigned char)lineText[charEnd] & 0xC0) == 0x80) charEnd++;
  lineText.erase(cursor_.col, charEnd - cursor_.col);
}

void TextEditor::newline() {
  pushUndo(false);
  deleteSelection();
  IndentStyle style = indentStyle();
  const std::string current = lines_[cursor_.line];
  std::string indent = leadingWhitespace(current);
  if (foldSyntax_ == FoldSyntax::C) {
    if ((size_t)cursor_.col <= indent.size()) {
      // Enter in the indentation: an empty line above, and the cursor stays before the code
      lines_.insert(lines_.begin() + cursor_.line, std::string());
      cursor_ = selStart_ = selEnd_ = anchor_ = {cursor_.line + 1, (int)indent.size()};
      scrollToCursor_ = true;
      return;
    }
    CNewline plan = cNewline(lines_, cursor_.line, cursor_.col, style, tabSize_);
    // what goes down to the new line loses the spaces it started with, and what stays loses
    // the ones it ended with
    std::string& line = lines_[cursor_.line];
    size_t restStart = cursor_.col;
    while (restStart < line.size() && (line[restStart] == ' ' || line[restStart] == '\t')) restStart++;
    size_t keptEnd = cursor_.col;
    while (keptEnd > indent.size() && (line[keptEnd - 1] == ' ' || line[keptEnd - 1] == '\t')) keptEnd--;
    line.erase(keptEnd, restStart - keptEnd);
    cursor_.col = (int)keptEnd;
    insertText("\n" + plan.indent + plan.prefix);
    if (plan.closeLine) {
      Coord inBlock = cursor_;
      insertText("\n" + plan.closeIndent + (plan.addBrace ? "}" : ""));
      cursor_ = selStart_ = selEnd_ = anchor_ = inBlock;
    }
    return;
  }
  // keep the indentation of the current line; after a label at column 0 the code still goes
  // one level in
  if ((size_t)cursor_.col < indent.size()) indent.resize(cursor_.col);
  bool hasLabels = foldSyntax_ == FoldSyntax::Asm || foldSyntax_ == FoldSyntax::Basic;
  if (hasLabels && indent.empty() && !current.empty() && !std::isspace((unsigned char)current[0]) && current[0] != ';')
    indent = style.unit();
  insertText("\n" + indent);
}

void TextEditor::typeText(const std::string& text) {
  pushUndo(text.size() == 1 && text[0] != ' ');
  deleteSelection();
  std::string& line = lines_[cursor_.line];
  bool onlyIndentBefore = leadingWhitespace(line).size() >= (size_t)cursor_.col;
  std::string indent;
  // a brace on a line of its own goes where the block it opens or closes says
  if (foldSyntax_ == FoldSyntax::C && (text == "}" || text == "{") && onlyIndentBefore &&
      cBraceIndent(lines_, cursor_.line, text[0], indent)) {
    line = indent + line.substr(leadingWhitespace(line).size());
    cursor_.col = (int)indent.size();
  }
  insertText(text);
}

void TextEditor::indentKey(bool outdent) {
  IndentStyle style = indentStyle();
  // one level less at the start of a line: a tab, or up to a level of spaces
  auto removeLevel = [&](std::string& line) {
    size_t remove = 0;
    if (!line.empty() && line[0] == '\t') remove = 1;
    else
      while (remove < line.size() && remove < (size_t)style.width && line[remove] == ' ') remove++;
    line.erase(0, remove);
    return (int)remove;
  };
  if (hasSelection() && selStart_.line != selEnd_.line) {
    pushUndo(false);
    auto [first, last] = selectedLines();
    for (int index = first; index <= last; index++) {
      if (outdent) removeLevel(lines_[index]);
      else if (!lines_[index].empty()) lines_[index].insert(0, style.unit());
    }
    selStart_ = {first, 0};
    selEnd_ = clamp({last, (int)lines_[last].size()});
    return;
  }
  pushUndo(false);
  if (outdent) {
    int removed = removeLevel(lines_[cursor_.line]);
    cursor_.col = std::max(0, cursor_.col - removed);
    selStart_ = selEnd_ = anchor_ = cursor_;
    return;
  }
  if (style.tabs) insertText("\t");
  else {
    int column = visualCol(cursor_.line, cursor_.col);
    insertText(std::string(style.width - column % style.width, ' '));  // to the next level
  }
}

void TextEditor::moveCursor(Coord target, bool select) {
  cursor_ = clamp(target);
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
  reveal(cursor_.line);
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
  const char* clipboard = ImGui::GetClipboardText();
  if (!clipboard || !*clipboard) return;
  pushUndo(false);
  insertText(clipboard);
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
  int wordStart;
  std::string word = wordAt({cursor_.line, cursor_.col}, &wordStart);
  std::string prefix = lines_[cursor_.line].substr(wordStart, cursor_.col - wordStart);
  if (prefix.size() < 2) return;
  completions_ = hooks_.complete(prefix, lines_[cursor_.line]);
  if (completions_.size() == 1 && completions_[0].word.size() == prefix.size()) completions_.clear();
  completionSel_ = 0;
  completionOpen_ = !completions_.empty();
}

void TextEditor::acceptCompletion() {
  if (!completionOpen_ || completions_.empty()) return;
  int wordStart;
  wordAt(cursor_, &wordStart);
  std::string word = completions_[completionSel_].word;
  // keep the case the user started typing in
  std::string typed = lines_[cursor_.line].substr(wordStart, cursor_.col - wordStart);
  bool lower = !typed.empty() && std::islower((unsigned char)typed[0]);
  if (lower && completions_[completionSel_].kind == TokenKind::Mnemonic)
    for (auto& character : word) character = (char)std::tolower((unsigned char)character);
  pushUndo(false);
  lines_[cursor_.line].replace(wordStart, cursor_.col - wordStart, word);
  cursor_.col = wordStart + (int)word.size();
  selStart_ = selEnd_ = anchor_ = cursor_;
  completionOpen_ = false;
}

void TextEditor::handleKeyboard() {
  ImGuiIO& io = ImGui::GetIO();
  bool shift = io.KeyShift, ctrl = io.KeyCtrl, alt = io.KeyAlt;
  bool wordMove = alt || ctrl;
  auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
  int pageLines = std::max(1, (int)(ImGui::GetWindowHeight() / lineH_) - 2);

  if (completionOpen_) {
    if (pressed(ImGuiKey_Escape)) { completionOpen_ = false; return; }
    if (pressed(ImGuiKey_DownArrow)) { completionSel_ = (completionSel_ + 1) % (int)completions_.size(); return; }
    if (pressed(ImGuiKey_UpArrow)) { completionSel_ = (completionSel_ + (int)completions_.size() - 1) % (int)completions_.size(); return; }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_Tab) || pressed(ImGuiKey_KeypadEnter)) { acceptCompletion(); return; }
  }

  if (pressed(ImGuiKey_F12) && hooks_.gotoDefinition) {
    std::string word = wordAt(cursor_);
    if (!word.empty()) hooks_.gotoDefinition(word);
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
#ifdef __APPLE__
  // ImGui reads Cmd as Ctrl on macOS, and Cmd+Space is Spotlight: completion is Control+Space
  // there (the Control key arrives as Super).
  if (io.KeySuper && !alt && !shift && pressed(ImGuiKey_Space)) { updateCompletion(); return; }
#endif

  Coord target = cursor_;
  if (pressed(ImGuiKey_LeftArrow)) {
    if (!shift && hasSelection()) { moveCursor(selStart_, false); return; }
    if (wordMove) {
      int wordStart; wordAt(target, &wordStart);
      target.col = wordStart == target.col ? std::max(0, target.col - 1) : wordStart;
      if (target.col == 0 && cursor_.col == 0 && target.line > 0) target = {target.line - 1, (int)lines_[target.line - 1].size()};
    } else if (target.col > 0) {
      target.col--;
      while (target.col > 0 && ((unsigned char)lines_[target.line][target.col] & 0xC0) == 0x80) target.col--;
    } else if (target.line > 0) target = {target.line - 1, (int)lines_[target.line - 1].size()};
    moveCursor(target, shift);
    completionOpen_ = false;
  } else if (pressed(ImGuiKey_RightArrow)) {
    if (!shift && hasSelection()) { moveCursor(selEnd_, false); return; }
    if (wordMove) {
      int wordEnd; wordAt(target, nullptr, &wordEnd);
      target.col = wordEnd == target.col ? target.col + 1 : wordEnd;
      if (target.col > (int)lines_[target.line].size() && target.line + 1 < (int)lines_.size()) target = {target.line + 1, 0};
    } else if (target.col < (int)lines_[target.line].size()) {
      target.col++;
      while (target.col < (int)lines_[target.line].size() && ((unsigned char)lines_[target.line][target.col] & 0xC0) == 0x80) target.col++;
    } else if (target.line + 1 < (int)lines_.size()) target = {target.line + 1, 0};
    moveCursor(target, shift);
    completionOpen_ = false;
  } else if (pressed(ImGuiKey_UpArrow) || pressed(ImGuiKey_DownArrow) || pressed(ImGuiKey_PageUp) || pressed(ImGuiKey_PageDown)) {
    int rowDelta = pressed(ImGuiKey_UpArrow) ? -1 : pressed(ImGuiKey_DownArrow) ? 1 : pressed(ImGuiKey_PageUp) ? -pageLines : pageLines;
    int visualColumn = visualCol(target.line, target.col);
    if (rowOf_.size() == lines_.size() && !lineAt_.empty())  // by rows on screen: folded regions are skipped
      target.line = lineAt_[std::clamp(rowOf_[target.line] + rowDelta, 0, (int)lineAt_.size() - 1)];
    else
      target.line = std::clamp(target.line + rowDelta, 0, (int)lines_.size() - 1);
    target.col = byteCol(target.line, visualColumn);
    moveCursor(target, shift);
  } else if (pressed(ImGuiKey_Home)) {
    int first = 0;
    while (first < (int)lines_[target.line].size() && std::isspace((unsigned char)lines_[target.line][first])) first++;
    target.col = target.col == first ? 0 : first;
    if (ctrl) target = {0, 0};
    moveCursor(target, shift);
  } else if (pressed(ImGuiKey_End)) {
    target.col = (int)lines_[target.line].size();
    if (ctrl) target = {(int)lines_.size() - 1, (int)lines_.back().size()};
    moveCursor(target, shift);
  } else if (pressed(ImGuiKey_Backspace)) {
    backspace();
    if (completionOpen_) updateCompletion();
  } else if (pressed(ImGuiKey_Delete)) {
    del();
  } else if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
    newline();
  } else if (pressed(ImGuiKey_Tab)) {
    indentKey(shift);
  } else if (pressed(ImGuiKey_Escape)) {
    completionOpen_ = false;
  }

  // typed characters
  if (!ctrl && io.InputQueueCharacters.Size > 0) {
    std::string typedText;
    for (ImWchar wideChar : io.InputQueueCharacters) {
      unsigned int codePoint = wideChar;
      if (codePoint < 32 || codePoint == 127) continue;
      if (codePoint < 0x80) typedText += (char)codePoint;
      else if (codePoint < 0x800) typedText += (char)(0xC0 | (codePoint >> 6)), typedText += (char)(0x80 | (codePoint & 0x3F));
      else typedText += (char)(0xE0 | (codePoint >> 12)), typedText += (char)(0x80 | ((codePoint >> 6) & 0x3F)), typedText += (char)(0x80 | (codePoint & 0x3F));
    }
    io.InputQueueCharacters.resize(0);
    if (!typedText.empty()) {
      typeText(typedText);
      if (isWordChar(typedText.back())) updateCompletion();
      else completionOpen_ = false;
    }
  }
}

// ---------------------------------------------------------------------------
// Search

static char lowerAscii(char character) { return (character >= 'A' && character <= 'Z') ? (char)(character + 32) : character; }

std::vector<Match> findMatches(const std::vector<std::string>& lines, const std::string& query, FindOptions options) {
  std::vector<Match> matchesFound;
  if (query.empty()) return matchesFound;
  std::string lowerQuery = query;
  if (!options.matchCase) for (auto& character : lowerQuery) character = lowerAscii(character);
  for (int i = 0; i < (int)lines.size(); i++) {
    std::string lineLower = lines[i];
    if (!options.matchCase) for (auto& character : lineLower) character = lowerAscii(character);
    for (size_t pos = lineLower.find(lowerQuery); pos != std::string::npos; pos = lineLower.find(lowerQuery, pos + 1)) {
      size_t end = pos + lowerQuery.size();
      if (options.wholeWord && ((pos > 0 && isWordChar(lineLower[pos - 1])) || (end < lineLower.size() && isWordChar(lineLower[end])))) continue;
      matchesFound.push_back({i, (int)pos, (int)end});
      pos = end - 1;  // no overlapping matches
    }
  }
  return matchesFound;
}

void TextEditor::refreshMatches() {
  if (bar_ == Bar::None || bar_ == Bar::Goto || findText_.empty()) {
    matches_.clear();
    currentMatch_ = -1;
    return;
  }
  if (matchesVersion_ == textVersion_ && matchesQuery_ == findText_ && matchesOptions_.matchCase == findOptions_.matchCase &&
      matchesOptions_.wholeWord == findOptions_.wholeWord)
    return;
  matches_ = findMatches(lines_, findText_, findOptions_);
  matchesVersion_ = textVersion_;
  matchesQuery_ = findText_;
  matchesOptions_ = findOptions_;
  currentMatch_ = -1;
  for (int i = 0; i < (int)matches_.size(); i++)
    if (hasSelection() && matches_[i].line == selStart_.line && matches_[i].start == selStart_.col) currentMatch_ = i;
}

void TextEditor::setSelection(Coord start, Coord end) {
  anchor_ = selStart_ = clamp(start);
  cursor_ = selEnd_ = clamp(end);
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
  for (auto& match : matches_)
    if (hasSelection() && selStart_ == Coord{match.line, match.start} && selEnd_ == Coord{match.line, match.end}) {
      pushUndo(false);
      lines_[match.line].replace(match.start, match.end - match.start, replaceText_);
      setSelection({match.line, match.start}, {match.line, match.start + (int)replaceText_.size()});
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
  int count = (int)matches_.size();
  for (int i = count - 1; i >= 0; i--) {
    auto& match = matches_[i];
    lines_[match.line].replace(match.start, match.end - match.start, replaceText_);
  }
  cursor_ = clamp(cursor_);
  selStart_ = selEnd_ = anchor_ = cursor_;
  refreshMatches();
  return count;
}

// ---------------------------------------------------------------------------
// Line editing

void TextEditor::toggleComment() {
  auto [firstLine, lastLine] = selectedLines();
  const std::string& prefix = commentPrefix_;
  bool allCommented = true, anyText = false;
  for (int i = firstLine; i <= lastLine; i++) {
    size_t firstChar = lines_[i].find_first_not_of(" \t");
    if (firstChar == std::string::npos) continue;
    anyText = true;
    if (lines_[i].compare(firstChar, prefix.size(), prefix) != 0) allCommented = false;
  }
  if (!anyText) return;
  pushUndo(false);
  for (int i = firstLine; i <= lastLine; i++) {
    std::string& lineText = lines_[i];
    size_t firstChar = lineText.find_first_not_of(" \t");
    if (firstChar == std::string::npos) continue;
    if (allCommented) {
      size_t prefixLength = prefix.size() + (lineText.compare(firstChar + prefix.size(), 1, " ") == 0 && prefix != ";" ? 1 : 0);
      lineText.erase(firstChar, prefixLength);
    } else {
      // DASM comments go at column 0; "rem" and "//" after the indentation
      // (in batari Basic a word at column 0 would be a label).
      if (prefix == ";") lineText.insert(0, prefix);
      else lineText.insert(firstChar, prefix + " ");
    }
  }
  if (hasSelection()) setSelection({firstLine, 0}, {lastLine, (int)lines_[lastLine].size()});
  else cursor_ = clamp(cursor_), selStart_ = selEnd_ = anchor_ = cursor_;
}

void TextEditor::duplicateLines() {
  auto [firstLine, lastLine] = selectedLines();
  pushUndo(false);
  std::vector<std::string> block(lines_.begin() + firstLine, lines_.begin() + lastLine + 1);
  lines_.insert(lines_.begin() + lastLine + 1, block.begin(), block.end());
  int lineCount = lastLine - firstLine + 1;
  if (hasSelection()) setSelection({selStart_.line + lineCount, selStart_.col}, {selEnd_.line + lineCount, selEnd_.col});
  else moveCursor({cursor_.line + lineCount, cursor_.col}, false);
}

void TextEditor::moveLines(int delta) {
  auto [firstLine, lastLine] = selectedLines();
  if ((delta < 0 && firstLine == 0) || (delta > 0 && lastLine + 1 >= (int)lines_.size())) return;
  pushUndo(false);
  if (delta < 0) {
    std::string above = lines_[firstLine - 1];
    lines_.erase(lines_.begin() + firstLine - 1);
    lines_.insert(lines_.begin() + lastLine, above);
  } else {
    std::string below = lines_[lastLine + 1];
    lines_.erase(lines_.begin() + lastLine + 1);
    lines_.insert(lines_.begin() + firstLine, below);
  }
  Coord newStart = selStart_, newEnd = selEnd_, newCursor = cursor_;
  newStart.line += delta, newEnd.line += delta, newCursor.line += delta;
  if (hasSelection()) setSelection(newStart, newEnd);
  else moveCursor(newCursor, false);
}

void TextEditor::deleteLines() {
  auto [firstLine, lastLine] = selectedLines();
  pushUndo(false);
  lines_.erase(lines_.begin() + firstLine, lines_.begin() + lastLine + 1);
  if (lines_.empty()) lines_.push_back("");
  moveCursor({std::min(firstLine, (int)lines_.size() - 1), 0}, false);
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
      int lineNumber = std::atoi(gotoText_.c_str());
      if (lineNumber > 0) goTo(lineNumber - 1), centerOnCursor_ = true;
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
    if (ImGui::Checkbox(T("Aa"), &findOptions_.matchCase)) matchesVersion_ = 0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Match case"));
    ImGui::SameLine();
    if (ImGui::Checkbox(T("Word"), &findOptions_.wholeWord)) matchesVersion_ = 0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Whole word"));
    ImGui::SameLine();
    refreshMatches();
    if (findText_.empty()) ImGui::TextDisabled(" ");
    else if (matches_.empty()) ImGui::TextColored(theme::colv(Role::MsgError), "%s", T("No results"));
    else ImGui::TextDisabled("%s", F("{0} of {1}", {currentMatch_ < 0 ? "?" : std::to_string(currentMatch_ + 1),
                                                       std::to_string(matches_.size())}).c_str());
    ImGui::SameLine();
    if (ImGui::ArrowButton("##prev", ImGuiDir_Up)) findNext(true);
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Down)) findNext(false);
    if (bar_ == Bar::Replace) {
      ImGui::SetNextItemWidth(280);
      bool replaceEntered = ImGui::InputTextWithHint("##replace", T("Replace"), &replaceText_, ImGuiInputTextFlags_EnterReturnsTrue);
      if (replaceEntered) {
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
  float foldW = foldSyntax_ != FoldSyntax::None ? charW_ * 1.6f : 0;  // fold arrows
  float gutterW = bpW + numW + cycW + foldW + charW_ * 1.5f;
  syncBreakpoints();
  updateRows();
  if (hiddenLine(cursor_.line)) reveal(cursor_.line);  // an edit, a search or a jump went inside a fold
  updateRows();

  int maxCols = 0;
  for (int i = 0; i < (int)lines_.size(); i++) maxCols = std::max(maxCols, visualCol(i, (int)lines_[i].size()));

  // Editor-wide shortcuts (work from the text and from the find bar).
  {
    ImGuiIO& shortcutIo = ImGui::GetIO();
    bool here = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (here && shortcutIo.KeyCtrl && !shortcutIo.KeyAlt) {
      if (ImGui::IsKeyPressed(ImGuiKey_F, false)) openFind(false);
      if (ImGui::IsKeyPressed(ImGuiKey_H, false) || (shortcutIo.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))) openFind(true);
      if (!shortcutIo.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G, false)) openGoto();  // Ctrl+Shift+G: go to address (app)
    }
    if (here && ImGui::IsKeyPressed(ImGuiKey_F9, false)) toggleBreakpoint(cursor_.line);
    // Ctrl/Cmd+Alt+Up folds the region around the cursor, +Down opens it
    if (here && shortcutIo.KeyCtrl && shortcutIo.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)) foldAt(cursor_.line), updateRows();
    if (here && shortcutIo.KeyCtrl && shortcutIo.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)) unfoldAt(cursor_.line), updateRows();
    if (here && ImGui::IsKeyPressed(ImGuiKey_F3)) {
      if (bar_ == Bar::None) openFind(false), focusBar_ = false;
      findNext(shortcutIo.KeyShift);
    }
  }
  renderFindBar();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, col(Role::EdBackground));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (focusEditor_) {
    ImGui::SetNextWindowFocus();
    focusEditor_ = false;
  }
  ImGuiIO& zoomIo = ImGui::GetIO();
  bool zoomWheel = zoomIo.KeyCtrl && zoomIo.MouseWheel != 0;  // the wheel zooms instead of scrolling
  ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs |
                        (zoomWheel ? ImGuiWindowFlags_NoScrollWithMouse : 0));
  if (zoomWheel && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) zoomSteps_ += zoomIo.MouseWheel > 0 ? 1 : -1;
  ImGui::PopStyleVar();

  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 contentSize(gutterW + (maxCols + 4) * charW_, lineAt_.size() * lineH_ + ImGui::GetWindowHeight() * 0.5f);
  ImGui::InvisibleButton("##text", ImVec2(std::max(contentSize.x, ImGui::GetContentRegionAvail().x), contentSize.y));
  bool hovered = ImGui::IsItemHovered();
  bool focused = ImGui::IsWindowFocused();
  ImGui::SetItemKeyOwner(ImGuiKey_Tab);
  ImGui::SetItemKeyOwner(ImGuiKey_UpArrow);
  ImGui::SetItemKeyOwner(ImGuiKey_DownArrow);

  ImGuiIO& io = ImGui::GetIO();
  float scrollX = ImGui::GetScrollX();
  float textX = origin.x + gutterW;

  // the line on a row of the screen, and the top of a line
  auto lineAtY = [&](float y) {
    int row = std::clamp((int)std::floor((y - origin.y) / lineH_), 0, (int)lineAt_.size() - 1);
    return lineAt_[row];
  };
  auto lineY = [&](int line) { return origin.y + rowOf_[std::clamp(line, 0, (int)rowOf_.size() - 1)] * lineH_; };
  auto mouseCoord = [&]() {
    ImVec2 mousePos = io.MousePos;
    int line = lineAtY(mousePos.y);
    int visualColumn = std::max(0, (int)std::lround((mousePos.x - textX) / charW_));
    return clamp({line, byteCol(line, visualColumn)});
  };

  // mouse
  float foldX = ImGui::GetWindowPos().x + bpW + numW + cycW;
  bool inMargin = io.MousePos.x < ImGui::GetWindowPos().x + bpW + numW;
  bool inFolds = foldW > 0 && io.MousePos.x >= foldX && io.MousePos.x < foldX + foldW + charW_ * 0.5f;
  bool inCycles = cycW > 0 && (hooks_.lineInfo || hooks_.gutterClick) && !inMargin && io.MousePos.x < foldX;
  if (hovered && inCycles && hooks_.gutterClick) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  // details only once the mouse rests on a line for a moment, so a click does not pop one up
  int restLine = hovered && inCycles ? lineAtY(io.MousePos.y) : -1;
  if (restLine != cycleRestLine_ || ImGui::IsMouseDown(ImGuiMouseButton_Left)) cycleRestLine_ = restLine, cycleRestTime_ = ImGui::GetTime();
  if (restLine >= 0 && hooks_.lineInfo && ImGui::GetTime() - cycleRestTime_ > 0.6) {
    std::string info = hooks_.lineInfo(lineAtY(io.MousePos.y));
    if (!info.empty()) ImGui::SetTooltip("%s", info.c_str());
  }
  if (hovered && inMargin && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    toggleBreakpoint(lineAtY(io.MousePos.y));
  } else if (hovered && inCycles && hooks_.gutterClick && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    hooks_.gutterClick();
  } else if (hovered && inFolds && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    int line = lineAtY(io.MousePos.y);
    if (regionAt(line)) {
      if (!unfoldAt(line)) foldAt(line);
      updateRows();
    }
  } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyCtrl && (hooks_.gotoDefinition || hooks_.openFile)) {
    Coord clicked = mouseCoord();
    moveCursor(clicked, false);
    std::string file;
    int fileStart = 0, fileEnd = 0;
    if (hooks_.openFile && fileNameInLine(lines_[clicked.line], &file, &fileStart, &fileEnd) && clicked.col >= fileStart && clicked.col <= fileEnd &&
        (!hooks_.hasFile || hooks_.hasFile(file))) {
      hooks_.openFile(file);
    } else if (hooks_.gotoDefinition) {
      std::string word = wordAt(clicked);
      if (!word.empty()) hooks_.gotoDefinition(word);
    }
  } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    Coord clicked = mouseCoord();
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      int wordStart, wordEnd;
      wordAt(clicked, &wordStart, &wordEnd);
      anchor_ = selStart_ = {clicked.line, wordStart};
      cursor_ = selEnd_ = {clicked.line, wordEnd};
    } else {
      if (!io.KeyShift) anchor_ = clicked;
      moveCursor(clicked, io.KeyShift);
    }
    completionOpen_ = false;
  } else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    moveCursor(mouseCoord(), true);
    scrollToCursor_ = true;
  }

  if (focused) handleKeyboard();
  updateRows();  // the keys may have changed the text

  // visible lines
  float scrollY = ImGui::GetScrollY();
  float viewH = ImGui::GetWindowHeight();
  int firstRow = std::max(0, (int)(scrollY / lineH_) - 1);
  int lastRow = std::min((int)lineAt_.size() - 1, (int)((scrollY + viewH) / lineH_) + 1);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  float fontSize = ImGui::GetFontSize();
  ImVec2 winPos = ImGui::GetWindowPos();

  // indentation: tabs are a level each; spaces go by the file's own step (what the Tab key
  // inserts: 8 in a DASM file whose conditionals sit 2 spaces in), or, in a file indented with
  // tabs, by the smallest indentation in sight (2-8, else 4)
  std::vector<ImU32> indentColours;
  for (auto guideColour : theme::current().indent)
    if (guideColour & 0xFF) indentColours.push_back(theme::u32(guideColour));
  ImU32 indentGuide = (theme::colour(Role::EdIndentGuide) & 0xFF) ? col(Role::EdIndentGuide) : 0;
  IndentStyle fileIndent = indentStyle();
  int spaceUnit = fileIndent.tabs ? 0 : fileIndent.width;
  for (int row = firstRow; row <= lastRow && fileIndent.tabs; row++) {
    int lineIndex = lineAt_[row];
    int spaceCount = 0;
    while (spaceCount < (int)lines_[lineIndex].size() && lines_[lineIndex][spaceCount] == ' ') spaceCount++;
    if (spaceCount > 0 && spaceCount < (int)lines_[lineIndex].size()) spaceUnit = spaceUnit ? std::min(spaceUnit, spaceCount) : spaceCount;
  }
  spaceUnit = spaceUnit >= 2 && spaceUnit <= 8 ? spaceUnit : 4;
  for (int row = firstRow; row <= lastRow; row++) {
    int lineIndex = lineAt_[row];
    float y = origin.y + row * lineH_;
    // current line
    if (lineIndex == cursor_.line && !hasSelection())
      drawList->AddRectFilled(ImVec2(winPos.x, y), ImVec2(winPos.x + ImGui::GetWindowWidth() + scrollX, y + lineH_), col(Role::EdCurrentLine));
    // indentation levels: a shade per level and a guide where each one starts
    if (!indentColours.empty() || indentGuide) {
      const std::string& indentText = lines_[lineIndex];
      int visualColumn = 0, level = 0;
      for (size_t byteIndex = 0; byteIndex < indentText.size() && (indentText[byteIndex] == ' ' || indentText[byteIndex] == '\t'); level++) {
        int from = visualColumn;
        if (indentText[byteIndex] == '\t') visualColumn = (visualColumn / tabSize_ + 1) * tabSize_, byteIndex++;
        else
          for (int spaceCount = 0; spaceCount < spaceUnit && byteIndex < indentText.size() && indentText[byteIndex] == ' '; spaceCount++) visualColumn++, byteIndex++;
        float x0 = textX + from * charW_, x1 = textX + visualColumn * charW_;
        if (!indentColours.empty()) drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + lineH_), indentColours[level % indentColours.size()]);
        if (indentGuide && level > 0) drawList->AddLine(ImVec2(x0, y), ImVec2(x0, y + lineH_), indentGuide);
      }
    }
    // marker background
    auto marker = markers_.find(lineIndex + 1);
    if (marker != markers_.end())
      drawList->AddRectFilled(ImVec2(textX, y), ImVec2(textX + contentSize.x, y + lineH_),
                        marker->second.first ? col(Role::EdErrorLine) : col(Role::EdWarningLine));
    // search matches
    for (int matchIndex = 0; matchIndex < (int)matches_.size(); matchIndex++) {
      const Match& match = matches_[matchIndex];
      if (match.line != lineIndex) continue;
      float x0 = textX + visualCol(lineIndex, match.start) * charW_, x1 = textX + visualCol(lineIndex, match.end) * charW_;
      drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + lineH_), matchIndex == currentMatch_ ? col(Role::EdMatchCurrent) : col(Role::EdMatch));
    }
    // selection
    if (hasSelection() && lineIndex >= selStart_.line && lineIndex <= selEnd_.line) {
      int startColumn = lineIndex == selStart_.line ? visualCol(lineIndex, selStart_.col) : 0;
      int endColumn = lineIndex == selEnd_.line ? visualCol(lineIndex, selEnd_.col) : visualCol(lineIndex, (int)lines_[lineIndex].size()) + 1;
      drawList->AddRectFilled(ImVec2(textX + startColumn * charW_, y), ImVec2(textX + endColumn * charW_, y + lineH_), col(Role::EdSelection));
    }
    // text
    const std::string& lineText = lines_[lineIndex];
    std::vector<Token> tokens = hooks_.tokenize ? hooks_.tokenize(lineText) : std::vector<Token>{};
    size_t tokenIndex = 0;
    int visualColumn = 0;
    for (int byteIndex = 0; byteIndex < (int)lineText.size();) {
      while (tokenIndex < tokens.size() && tokens[tokenIndex].end <= byteIndex) tokenIndex++;
      TokenKind tokenKind = (tokenIndex < tokens.size() && tokens[tokenIndex].start <= byteIndex) ? tokens[tokenIndex].kind : TokenKind::Text;
      unsigned char character = lineText[byteIndex];
      if (character == '\t') {
        visualColumn = (visualColumn / tabSize_ + 1) * tabSize_;
        byteIndex++;
        continue;
      }
      int charLength = 1;
      if (character >= 0xF0) charLength = 4; else if (character >= 0xE0) charLength = 3; else if (character >= 0xC0) charLength = 2;
      drawList->AddText(font, fontSize, ImVec2(textX + visualColumn * charW_, y + 1), colorOf(tokenKind), lineText.data() + byteIndex, lineText.data() + std::min((int)lineText.size(), byteIndex + charLength));
      visualColumn++;
      byteIndex += charLength;
    }
    // a folded region: a box after the line says how many lines it hides
    if (folds_.count(lineIndex))
      if (const FoldRegion* foldRegion = regionAt(lineIndex)) {
        std::string tag = " ... " + std::to_string(foldRegion->last - foldRegion->first) + " ";
        float x0 = textX + (visualColumn + 1) * charW_, x1 = x0 + ImGui::CalcTextSize(tag.c_str()).x;
        drawList->AddRectFilled(ImVec2(x0, y + 1), ImVec2(x1, y + lineH_ - 1), col(Role::EdMatch), lineH_ * 0.25f);
        drawList->AddText(font, fontSize, ImVec2(x0, y + 1), col(Role::EdLineNumber), tag.c_str());
      }
  }

  // Ctrl/Cmd over a symbol with a definition: underlined like a link, hand cursor.
  if (hovered && io.KeyCtrl && !io.KeyAlt && !inMargin && (hooks_.gotoDefinition || hooks_.openFile) && io.MousePos.x > textX) {
    int line = lineAtY(io.MousePos.y);
    if (line >= 0 && line < (int)lines_.size()) {
      int linkStart = 0, linkEnd = 0;
      Coord hoverPos = clamp({line, byteCol(line, (int)((io.MousePos.x - textX) / charW_))});
      std::string hoveredWord, file;
      int fileStart = 0, fileEnd = 0;
      bool onFile = hooks_.openFile && fileNameInLine(lines_[line], &file, &fileStart, &fileEnd) && hoverPos.col >= fileStart && hoverPos.col <= fileEnd;
      if (onFile) {
        hoveredWord = "\x01" + file, linkStart = fileStart, linkEnd = fileEnd;  // a file name: its own cache key
      } else if (hooks_.gotoDefinition) {
        hoveredWord = wordAt(hoverPos, &linkStart, &linkEnd);
        if (!hoveredWord.empty() && std::isdigit((unsigned char)hoveredWord[0])) hoveredWord.clear();
      }
      if (hoveredWord != linkWord_ || line != linkLine_) {
        linkWord_ = hoveredWord, linkLine_ = line;
        if (onFile) linkOk_ = !hooks_.hasFile || hooks_.hasFile(file);
        else linkOk_ = !hoveredWord.empty() && (!hooks_.hasDefinition || hooks_.hasDefinition(hoveredWord, line));
      }
      if (linkOk_) {
        float x0 = textX + visualCol(line, linkStart) * charW_, x1 = textX + visualCol(line, linkEnd) * charW_;
        float y = lineY(line) + lineH_ - 1;
        drawList->AddLine(ImVec2(x0, y), ImVec2(x1, y), col(Role::EdLink), 1.0f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
    }
  } else {
    linkWord_.clear(), linkLine_ = -1;
  }

  // separators: a rule above the line with its label at the right, or (a page that starts inside
  // a line, such as an instruction or a table that crosses it) a label with an arrow to that line
  for (int row = firstRow; row <= lastRow; row++) {
    int lineIndex = lineAt_[row];
    auto [first, last] = separators_.equal_range(lineIndex);
    for (auto separator = first; separator != last; ++separator) {
      float y = origin.y + row * lineH_;
      float right = winPos.x + ImGui::GetWindowWidth() - charW_ * 2;
      const std::string& label = separator->second.label;
      ImU32 ruleColour = col(separator->second.bank ? Role::EdBankRule : Role::EdPageRule);
      float thickness = std::max(1.0f, charW_ * (separator->second.bank ? 0.28f : 0.16f));
      float labelWidth = ImGui::CalcTextSize(label.c_str()).x * 0.9f;
      float pad = charW_ * 0.6f, round = lineH_ * 0.2f;
      int endCols = visualCol(lineIndex, (int)lines_[lineIndex].size());
      if (separator->second.inside) {
        float arrowTip = textX + endCols * charW_ + charW_ * 0.5f;  // the end of the line: no stroke through its code or comment
        float middle = y + lineH_ * 0.5f;
        // the label right of the line, with an arrow to it
        float labelX = textX + (endCols + 4) * charW_;
        drawList->AddLine(ImVec2(arrowTip, middle), ImVec2(labelX - pad, middle), ruleColour, thickness);
        drawList->AddTriangleFilled(ImVec2(arrowTip, middle), ImVec2(arrowTip + charW_ * 0.6f, middle - lineH_ * 0.2f),
                                    ImVec2(arrowTip + charW_ * 0.6f, middle + lineH_ * 0.2f), ruleColour);
        drawList->AddRectFilled(ImVec2(labelX - pad, middle - lineH_ * 0.45f), ImVec2(labelX + labelWidth + pad, middle + lineH_ * 0.45f), col(Role::EdBackground), round);
        drawList->AddRect(ImVec2(labelX - pad, middle - lineH_ * 0.45f), ImVec2(labelX + labelWidth + pad, middle + lineH_ * 0.45f), ruleColour, round, 0, 1.0f);
        drawList->AddText(font, fontSize * 0.9f, ImVec2(labelX, middle - lineH_ * 0.42f), ruleColour, label.c_str());
        continue;
      }
      // the label goes right of the code on both sides of the rule, or at the right edge
      int besideCols = endCols;
      if (row > 0) besideCols = std::max(besideCols, visualCol(lineAt_[row - 1], (int)lines_[lineAt_[row - 1]].size()));
      float labelX = std::max(right - labelWidth, textX + (besideCols + 3) * charW_);
      // a coloured rule across the code, and its label in the same colour on a patch of background
      drawList->AddLine(ImVec2(winPos.x + gutterW - charW_ * 0.5f, y), ImVec2(winPos.x + ImGui::GetWindowWidth(), y), ruleColour, thickness);
      drawList->AddRectFilled(ImVec2(labelX - pad, y - lineH_ * 0.45f), ImVec2(labelX + labelWidth + pad, y + lineH_ * 0.45f), col(Role::EdBackground), round);
      drawList->AddRect(ImVec2(labelX - pad, y - lineH_ * 0.45f), ImVec2(labelX + labelWidth + pad, y + lineH_ * 0.45f), ruleColour, round, 0, 1.0f);
      drawList->AddText(font, fontSize * 0.9f, ImVec2(labelX, y - lineH_ * 0.42f), ruleColour, label.c_str());
    }
  }

  // gutter (drawn over the text so it stays fixed when scrolling horizontally)
  float gx = winPos.x;
  drawList->AddRectFilled(ImVec2(gx, winPos.y), ImVec2(gx + gutterW - charW_ * 0.5f, winPos.y + viewH), col(Role::EdGutter));
  for (int row = firstRow; row <= lastRow; row++) {
    int lineIndex = lineAt_[row];
    float y = origin.y + row * lineH_;
    char lineNumberText[32];
    snprintf(lineNumberText, sizeof lineNumberText, "%*d", std::min(numChars - 1, 12), lineIndex + 1);
    auto marker = markers_.find(lineIndex + 1);
    ImU32 numCol = marker != markers_.end() ? (marker->second.first ? col(Role::EdErrorNumber) : col(Role::EdWarningNumber))
                   : lineIndex == cursor_.line ? col(Role::EdLineNumberCurrent) : col(Role::EdLineNumber);
    if (bps_.count(lineIndex))
      drawList->AddCircleFilled(ImVec2(gx + bpW * 0.5f, y + lineH_ * 0.5f), std::min(bpW, lineH_) * 0.32f, col(Role::EdBreakpoint));
    drawList->AddText(font, fontSize, ImVec2(gx + bpW, y + 1), numCol, lineNumberText);
    if (cycW > 0 && hooks_.gutter) {
      std::string gutterText = hooks_.gutter(lineIndex, lines_[lineIndex]);
      if (!gutterText.empty()) {
        float textWidth = ImGui::CalcTextSize(gutterText.c_str()).x;
        drawList->AddText(font, fontSize, ImVec2(gx + bpW + numW + cycW - textWidth, y + 1), col(Role::EdCycles), gutterText.c_str());
      }
    }
    // fold arrow: always on a folded region, on the others while the mouse is over the gutter
    if (foldW > 0 && regionAt(lineIndex)) {
      bool isFolded = folds_.count(lineIndex) > 0;
      bool overGutter = hovered && io.MousePos.x < gx + gutterW;
      if (isFolded || overGutter) {
        float cx = gx + bpW + numW + cycW + foldW * 0.5f, cy = y + lineH_ * 0.5f, arrowSize = std::min(foldW, lineH_) * 0.22f;
        ImU32 arrowColour = isFolded ? col(Role::EdLineNumberCurrent) : col(Role::EdLineNumber);
        if (isFolded) drawList->AddTriangleFilled(ImVec2(cx - arrowSize * 0.6f, cy - arrowSize), ImVec2(cx - arrowSize * 0.6f, cy + arrowSize), ImVec2(cx + arrowSize * 0.9f, cy), arrowColour);
        else drawList->AddTriangleFilled(ImVec2(cx - arrowSize, cy - arrowSize * 0.6f), ImVec2(cx + arrowSize, cy - arrowSize * 0.6f), ImVec2(cx, cy + arrowSize * 0.9f), arrowColour);
      }
    }
  }
  drawList->AddLine(ImVec2(gx + gutterW - charW_ * 0.5f, winPos.y), ImVec2(gx + gutterW - charW_ * 0.5f, winPos.y + viewH), col(Role::EdGutterBorder));
  // offset the gutter by the horizontal scroll so it stays on screen
  (void)scrollX;

  // cursor
  if (focused) {
    float cx = textX + visualCol(cursor_.line, cursor_.col) * charW_;
    float cy = lineY(cursor_.line);
    if (std::fmod(ImGui::GetTime(), 1.0) < 0.6 || io.InputQueueCharacters.Size)
      drawList->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + 2, cy + lineH_), col(Role::EdCursor));
    // completion popup
    if (completionOpen_) {
      ImGui::SetNextWindowPos(ImVec2(cx, cy + lineH_));
      ImGui::SetNextWindowBgAlpha(0.97f);
      ImGui::BeginTooltip();
      for (int entryIndex = 0; entryIndex < (int)completions_.size() && entryIndex < 12; entryIndex++) {
        bool selected = entryIndex == completionSel_;
        ImGui::PushStyleColor(ImGuiCol_Text, colorOf(completions_[entryIndex].kind));
        ImGui::Selectable(completions_[entryIndex].word.c_str(), selected, 0, ImVec2(charW_ * 10, 0));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", completions_[entryIndex].detail.c_str());
      }
      ImGui::EndTooltip();
    }
  }

  // hover help
  if (hovered && hooks_.hover && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !completionOpen_ &&
      ImGui::GetIO().MouseDelta.x == 0 && ImGui::GetIO().MouseDelta.y == 0) {
    ImVec2 mousePos = io.MousePos;
    int line = lineAtY(mousePos.y);
    if (line >= 0 && line < (int)lines_.size() && mousePos.x > textX) {
      Coord hoverCoord = clamp({line, byteCol(line, (int)((mousePos.x - textX) / charW_))});
      std::string hoveredWord = wordAt(hoverCoord);
      auto marker = markers_.find(line + 1);
      std::string help = hoveredWord.empty() ? std::string() : hooks_.hover(hoveredWord, lines_[line]);
      if (marker != markers_.end()) help = marker->second.second + (help.empty() ? "" : "\n\n" + help);
      if (!help.empty() && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(charW_ * 70);
        ImGui::TextUnformatted(help.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
    }
  }

  if (scrollToCursor_) {
    float cy = rowOf_[std::clamp(cursor_.line, 0, (int)rowOf_.size() - 1)] * lineH_;
    if (centerOnCursor_ && (cy < scrollY + lineH_ * 2 || cy > scrollY + viewH - lineH_ * 3))
      ImGui::SetScrollY(std::max(0.0f, cy - viewH * 0.4f));
    else if (cy < scrollY) ImGui::SetScrollY(cy);
    else if (cy + lineH_ * 2 > scrollY + viewH) ImGui::SetScrollY(cy + lineH_ * 2 - viewH);
    float cx = gutterW + visualCol(cursor_.line, cursor_.col) * charW_;
    float viewWidth = ImGui::GetWindowWidth();
    if (cx - gutterW < scrollX) ImGui::SetScrollX(std::max(0.0f, cx - gutterW - charW_ * 4));
    else if (cx > scrollX + viewWidth - charW_ * 2) ImGui::SetScrollX(cx - viewWidth + charW_ * 4);
    scrollToCursor_ = false;
    centerOnCursor_ = false;
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopFont();
  return changed_;
}

}  // namespace vcs
