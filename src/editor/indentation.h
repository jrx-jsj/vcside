// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Indentation for the code editor: how a file indents (tabs or spaces, and how many), and
// what a new line gets in C: one level more after "{" or a braceless "if (...)", the "}"
// that closes a block, and the " * " that continues a block comment.
#pragma once
#include <string>
#include <vector>

namespace vcs {

// How a file indents one level: a tab, or a number of spaces.
struct IndentStyle {
  bool tabs = true;
  int width = 4;  // spaces per level when not tabs
  std::string unit() const { return tabs ? "\t" : std::string(width, ' '); }
  bool operator==(const IndentStyle& other) const { return tabs == other.tabs && (tabs || width == other.width); }
};

// The style most of the file's indented lines use; `fallback` when no line is indented.
// Spaces: the step most often seen from one line to the next more indented one (2 to 8).
IndentStyle detectIndentStyle(const std::vector<std::string>& lines, IndentStyle fallback);

// The spaces and tabs a line starts with.
std::string leadingWhitespace(const std::string& line);
// The width of an indentation in columns, with tabs to the next multiple of tabSize.
int indentColumns(const std::string& whitespace, int tabSize);

// C: a line's code without its comments, and with strings and characters left empty ("" and
// ''), so braces in them do not count. `inBlockComment` says whether a /* */ comment is open
// before the line, and is updated for the next one.
std::string cCode(const std::string& line, bool& inBlockComment);

// C: what Enter does when it splits `lines[line]` at byte `column`. The new line starts with
// `indent` and `prefix`, then the cursor. With `closeLine`, another line follows the cursor's,
// at `closeIndent`: it takes the "}" that was after the cursor ("{|}"), or a "}" of its own
// (`addBrace`) when the block that "{" opens is not closed yet. Whatever was after the cursor
// goes after all that, without the spaces it started with.
struct CNewline {
  std::string indent, prefix;
  bool closeLine = false, addBrace = false;
  std::string closeIndent;
};
CNewline cNewline(const std::vector<std::string>& lines, int line, int column, IndentStyle style, int tabSize);

// C: the indentation a line gets when "}" (or "{") is typed into it and it held only spaces
// and tabs; `brace` is the character typed. Returns false to leave the line as it is.
//  "}": the indentation of the line with the "{" it closes.
//  "{": the indentation of a braceless "if (...)", "for (...)", "else"... on the line above
//       (the brace on a line of its own goes under the statement it belongs to).
bool cBraceIndent(const std::vector<std::string>& lines, int line, char brace, std::string& indent);

// Generated code (graphics and music exports) indented with tabs, as the templates: each line's
// leading spaces become one tab per `unit` spaces. roundUp: a part of a unit is one tab more
// (DASM: conditionals 2 spaces in, code 4 or 8 in, all one tab); otherwise the rest stays as
// spaces (C: the " *" of a block comment).
inline std::string indentWithTabs(const std::string& text, int unit, bool roundUp) {
  std::string out;
  out.reserve(text.size());
  bool lineStart = true;
  size_t spaces = 0;
  for (char character : text) {
    if (lineStart && character == ' ') {
      spaces++;
      continue;
    }
    if (lineStart) {
      bool blank = character == '\n';
      if (!blank && spaces) {
        size_t tabs = spaces / unit, rest = spaces % unit;
        if (roundUp && rest) tabs++, rest = 0;
        out.append(tabs, '\t').append(rest, ' ');
      }
      lineStart = false, spaces = 0;
    }
    out += character;
    if (character == '\n') lineStart = true;
  }
  return out;
}

}  // namespace vcs
