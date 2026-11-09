// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "indentation.h"

#include <cctype>
#include <map>

namespace vcs {

namespace {

bool isBlank(char character) { return character == ' ' || character == '\t'; }

std::string trimRight(const std::string& text) {
  size_t end = text.size();
  while (end > 0 && (isBlank(text[end - 1]) || text[end - 1] == '\r')) end--;
  return text.substr(0, end);
}

std::string trimLeft(const std::string& text) {
  size_t start = 0;
  while (start < text.size() && isBlank(text[start])) start++;
  return text.substr(start);
}

bool isBlankLine(const std::string& line) { return trimLeft(trimRight(line)).empty(); }

bool startsWithWord(const std::string& text, const std::string& word) {
  if (text.compare(0, word.size(), word) != 0) return false;
  if (text.size() == word.size()) return true;
  char next = text[word.size()];
  return !(std::isalnum((unsigned char)next) || next == '_');
}

// The C code of each line before `line`, and whether a block comment is open where it starts.
std::vector<std::string> codeBefore(const std::vector<std::string>& lines, int line, bool& inBlockComment) {
  std::vector<std::string> code;
  inBlockComment = false;
  for (int index = 0; index < line && index < (int)lines.size(); index++) code.push_back(cCode(lines[index], inBlockComment));
  return code;
}

// A control statement whose body is the next statement, without braces: "if (...)",
// "else if (...)", "for (...)", "while (...)", "else", "do" (also after a "}").
bool isBracelessHeader(std::string code) {
  code = trimRight(trimLeft(code));
  if (!code.empty() && code[0] == '}') code = trimLeft(code.substr(1));
  if (code == "else" || code == "do") return true;
  if (startsWithWord(code, "else")) code = trimLeft(code.substr(4));
  bool control = startsWithWord(code, "if") || startsWithWord(code, "for") || startsWithWord(code, "while");
  if (!control) return false;
  size_t open = code.find('(');
  if (open == std::string::npos) return false;
  int depth = 0;
  for (size_t position = open; position < code.size(); position++) {
    if (code[position] == '(') depth++;
    else if (code[position] == ')' && --depth == 0) return position == code.size() - 1;  // the condition ends the line
  }
  return false;
}

// The nearest line above `line` with code, or -1.
int previousCodeLine(const std::vector<std::string>& code, int line) {
  for (int index = line - 1; index >= 0; index--)
    if (!trimLeft(trimRight(code[index])).empty()) return index;
  return -1;
}

// Whether the block opened on `line` is already closed: the next line that is not more
// indented than it starts with "}" at the same indentation.
bool blockClosed(const std::vector<std::string>& lines, int line, int tabSize) {
  int columns = indentColumns(leadingWhitespace(lines[line]), tabSize);
  for (int index = line + 1; index < (int)lines.size(); index++) {
    if (isBlankLine(lines[index])) continue;
    int lineColumns = indentColumns(leadingWhitespace(lines[index]), tabSize);
    if (lineColumns > columns) continue;
    return lineColumns == columns && trimLeft(lines[index])[0] == '}';
  }
  return false;
}

}  // namespace

IndentStyle detectIndentStyle(const std::vector<std::string>& lines, IndentStyle fallback) {
  int tabLines = 0, spaceLines = 0, previousSpaces = 0;
  std::map<int, int> steps;  // how often a line is indented this many spaces more than the one before
  for (const std::string& line : lines) {
    if (isBlankLine(line)) continue;
    std::string whitespace = leadingWhitespace(line);
    if (line.compare(whitespace.size(), 1, "*") == 0) continue;  // inside a block comment
    if (!whitespace.empty() && whitespace[0] == '\t') {
      tabLines++;
      continue;
    }
    int spaces = (int)whitespace.size();
    if (spaces > 0) spaceLines++;
    int step = spaces - previousSpaces;
    if (step >= 2 && step <= 8) steps[step]++;
    previousSpaces = spaces;
  }
  if (tabLines == 0 && spaceLines == 0) return fallback;
  if (tabLines >= spaceLines) return {true, fallback.width};
  IndentStyle style{false, fallback.tabs ? 4 : fallback.width};
  int seen = 0;
  for (auto [step, count] : steps)
    if (count > seen) style.width = step, seen = count;  // the smaller step wins a tie
  return style;
}

std::string leadingWhitespace(const std::string& line) {
  size_t end = 0;
  while (end < line.size() && isBlank(line[end])) end++;
  return line.substr(0, end);
}

int indentColumns(const std::string& whitespace, int tabSize) {
  int columns = 0;
  for (char character : whitespace) columns = character == '\t' ? (columns / tabSize + 1) * tabSize : columns + 1;
  return columns;
}

std::string cCode(const std::string& line, bool& inBlockComment) {
  std::string code;
  for (size_t position = 0; position < line.size(); position++) {
    char character = line[position];
    char next = position + 1 < line.size() ? line[position + 1] : 0;
    if (inBlockComment) {
      if (character == '*' && next == '/') inBlockComment = false, position++;
      continue;
    }
    if (character == '/' && next == '*') {
      inBlockComment = true, position++;
      continue;
    }
    if (character == '/' && next == '/') break;
    if (character == '"' || character == '\'') {
      code += character;
      for (position++; position < line.size() && line[position] != character; position++)
        if (line[position] == '\\') position++;
      code += character;
      continue;
    }
    code += character;
  }
  return code;
}

CNewline cNewline(const std::vector<std::string>& lines, int line, int column, IndentStyle style, int tabSize) {
  CNewline result;
  const std::string& text = lines[line];
  std::string before = text.substr(0, column), after = text.substr(column);
  std::string indent = leadingWhitespace(text);
  if ((int)indent.size() > column) indent = indent.substr(0, column);
  result.indent = indent;

  bool inBlockComment = false;
  std::vector<std::string> code = codeBefore(lines, line, inBlockComment);
  std::string codeToCursor = trimRight(cCode(before, inBlockComment));

  // in a block comment: its lines go on with " * "
  if (inBlockComment) {
    std::string start = trimLeft(before);
    if (start.compare(0, 2, "/*") == 0) result.prefix = " * ";
    else if (start.compare(0, 1, "*") == 0) result.prefix = "* ";
    return result;
  }

  if (!codeToCursor.empty() && codeToCursor.back() == '{') {
    result.indent = indent + style.unit();
    std::string rest = trimLeft(after);
    if (!rest.empty() && rest[0] == '}') {
      result.closeLine = true;  // "{|}": the "}" goes to a line of its own
      result.closeIndent = indent;
    } else if (rest.empty() && !blockClosed(lines, line, tabSize)) {
      result.closeLine = result.addBrace = true;
      result.closeIndent = indent;
    }
    return result;
  }
  if (isBracelessHeader(codeToCursor)) {
    result.indent = indent + style.unit();
    return result;
  }
  // the one statement of a braceless "if (...)" ends: back to the "if"
  if (!codeToCursor.empty() && codeToCursor.back() == ';') {
    code.push_back(codeToCursor);
    int header = previousCodeLine(code, line);
    if (header >= 0 && isBracelessHeader(code[header])) {
      std::string headerIndent = leadingWhitespace(lines[header]);
      if (indentColumns(headerIndent, tabSize) < indentColumns(indent, tabSize)) result.indent = headerIndent;
    }
  }
  return result;
}

bool cBraceIndent(const std::vector<std::string>& lines, int line, char brace, std::string& indent) {
  bool inBlockComment = false;
  std::vector<std::string> code = codeBefore(lines, line, inBlockComment);
  if (inBlockComment) return false;
  if (brace == '{') {
    int header = previousCodeLine(code, line);
    if (header < 0 || !isBracelessHeader(code[header])) return false;
    indent = leadingWhitespace(lines[header]);
    return true;
  }
  int depth = 0;
  for (int index = line - 1; index >= 0; index--) {
    for (int position = (int)code[index].size() - 1; position >= 0; position--) {
      if (code[index][position] == '}') depth++;
      else if (code[index][position] == '{' && depth-- == 0) {
        indent = leadingWhitespace(lines[index]);
        return true;
      }
    }
  }
  return false;
}

}  // namespace vcs
