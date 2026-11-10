// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/validate.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace vcs::assistant {

namespace {
bool identifierCharacter(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return text;
}

// the text of the answer that is code: fenced blocks and backtick spans
std::vector<std::pair<std::string, bool>> codeParts(const std::string& answer) {  // text, and whether it is a fenced block
  std::vector<std::pair<std::string, bool>> parts;
  size_t position = 0;
  while (position < answer.size()) {
    size_t fence = answer.find("```", position);
    size_t tick = answer.find('`', position);
    if (tick == std::string::npos) break;
    if (fence == tick) {  // a block
      size_t lineEnd = answer.find('\n', fence);
      if (lineEnd == std::string::npos) break;
      size_t close = answer.find("```", lineEnd);
      parts.push_back({answer.substr(lineEnd + 1, (close == std::string::npos ? answer.size() : close) - lineEnd - 1), true});
      if (close == std::string::npos) break;
      position = close + 3;
    } else {  // inline code
      size_t close = answer.find('`', tick + 1);
      if (close == std::string::npos) break;
      parts.push_back({answer.substr(tick + 1, close - tick - 1), false});
      position = close + 1;
    }
  }
  return parts;
}

const std::set<std::string>& directives() {
  static const std::set<std::string> words = {"PROCESSOR", "INCLUDE", "ORG", "SEG", "MAC", "ENDM", "IF", "ENDIF", "ELSE", "ECHO", "ALIGN", "RORG", "REPEAT", "REPEND", "DS", "BYTE", "WORD", "HEX",
                                              "EQU", "SET", "END", "SUBROUTINE", "INCBIN", "LIST", "DC", "DV", "ERR", "EOF", "SEGU", "LONG", "DATA", "REM", "DIM", "CONST", "GOSUB", "GOTO",
                                              "RETURN", "THEN", "TRUE", "FALSE", "NULL", "ARM", "TODO", "NOTE", "FIXME", "XXX"};
  return words;
}
}  // namespace

std::vector<std::string> unknownNames(const std::string& answer, const KnowledgeBase& kb, const std::function<bool(const std::string&)>& inProject, size_t limit, bool batariBasic) {
  std::vector<std::string> unknown;
  std::set<std::string> defined, reported;
  auto parts = codeParts(answer);
  // names the answer defines itself: NAME = x, NAME EQU x, NAME SET x, NAME DS n, a label at the start of a line
  for (auto& item : parts) {
    if (!item.second) continue;  // a name in `backticks` on its own is a mention, not a label
    const std::string& part = item.first;
    size_t lineStart = 0;
    while (lineStart <= part.size()) {
      size_t lineEnd = part.find('\n', lineStart);
      if (lineEnd == std::string::npos) lineEnd = part.size();
      std::string line = part.substr(lineStart, lineEnd - lineStart);
      size_t end = 0;
      while (end < line.size() && identifierCharacter(line[end])) end++;
      if (end > 0 && !std::isspace((unsigned char)line[0])) {
        std::string name = line.substr(0, end);
        size_t after = line.find_first_not_of(" \t", end);
        bool assignment = after != std::string::npos && (line[after] == '=' || line.compare(after, 3, "EQU") == 0 || line.compare(after, 3, "equ") == 0 || line.compare(after, 3, "SET") == 0 ||
                                                           line.compare(after, 2, "DS") == 0 || line.compare(after, 2, "ds") == 0);
        if (assignment || after == std::string::npos || line[after] == ';' || line[after] == ':') defined.insert(name);  // a name alone on its line is a label
      }
      if (lineEnd == part.size()) break;
      lineStart = lineEnd + 1;
    }
  }
  for (auto& item : parts) {
    const std::string& part = item.first;
    size_t position = 0;
    while (position < part.size()) {
      if (!identifierCharacter(part[position]) || (position > 0 && (identifierCharacter(part[position - 1]) || part[position - 1] == '$' || part[position - 1] == '%'))) {
        position++;
        continue;
      }
      size_t end = position;
      while (end < part.size() && identifierCharacter(part[end])) end++;
      std::string word = part.substr(position, end - position);
      position = end;
      if (word.size() < 3 || std::isdigit((unsigned char)word[0])) continue;
      bool upperCase = std::all_of(word.begin(), word.end(), [](char c) { return !std::islower((unsigned char)c); }) &&
                       std::any_of(word.begin(), word.end(), [](char c) { return std::isupper((unsigned char)c); });
      if (!upperCase) continue;
      if (reported.count(word) || defined.count(word) || directives().count(word)) continue;
      if (kb.reg(word) || kb.instruction(word) || kb.format(word) || kb.tv(word) || (batariBasic && (kb.basic(lowered(word)) || kb.basic(word)))) continue;
      if (inProject && inProject(lowered(word))) continue;
      reported.insert(word);
      unknown.push_back(word);
      if (unknown.size() >= limit) return unknown;
    }
  }
  return unknown;
}

}  // namespace vcs::assistant
