// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "basic_syntax.h"

#include <cctype>

namespace vcs {

static bool wordChar(char character) { return std::isalnum((unsigned char)character) || character == '_'; }

std::string basicLabelOf(const std::string& line) {
  if (line.empty() || !wordChar(line[0])) return {};
  size_t labelEnd = 0;
  while (labelEnd < line.size() && wordChar(line[labelEnd])) labelEnd++;
  if (labelEnd < line.size() && line[labelEnd] == ':') return {};  // a block such as player0: or playfield:
  return line.substr(0, labelEnd);
}

std::vector<Token> tokenizeBasic(const KnowledgeBase& kb, const std::string& line) {
  std::vector<Token> tokens;
  int length = (int)line.size(), i = 0;
  bool jumpTarget = false;  // after goto, gosub, then: a bare word is a label
  bool afterThen = false;
  if (!basicLabelOf(line).empty()) {
    while (i < length && wordChar(line[i])) i++;
    tokens.push_back({0, i, TokenKind::Label});
  }
  while (i < length) {
    char character = line[i];
    if (std::isspace((unsigned char)character)) { i++; continue; }
    int tokenStart = i;
    if (character == '"') {
      i++;
      while (i < length && line[i] != '"') i++;
      i = std::min(length, i + 1);
      tokens.push_back({tokenStart, i, TokenKind::String});
    } else if (character == '$' || character == '%' || std::isdigit((unsigned char)character)) {
      i++;
      while (i < length && (wordChar(line[i]) || line[i] == '.')) i++;  // also 8kSC
      tokens.push_back({tokenStart, i, TokenKind::Number});
      jumpTarget = jumpTarget && std::isdigit((unsigned char)character);  // goto 100
      afterThen = false;
    } else if (std::isalpha((unsigned char)character) || character == '_') {
      while (i < length && wordChar(line[i])) i++;
      std::string word = line.substr(tokenStart, i - tokenStart);
      if (word == "rem") {
        tokens.push_back({tokenStart, length, TokenKind::Comment});
        break;
      }
      int wordEnd = i;
      if (i < length && line[i] == ':' && kb.basic(word + ":")) word += ":", wordEnd = ++i;
      TokenKind kind = TokenKind::Symbol;
      bool jump = false;
      if (const BasicKeyword* keyword = kb.basic(word)) {
        if (keyword->kind == "variable") kind = TokenKind::Register;
        else if (keyword->kind == "directive" || keyword->kind == "block") kind = TokenKind::Directive;
        else kind = TokenKind::Mnemonic;
        jump = word == "goto" || word == "gosub" || word == "then" || word == "else";
      } else if (kb.reg(word)) {
        kind = TokenKind::Register;
      } else if (jumpTarget) {
        // after then, only a word alone is a label (then a = 1 is a statement)
        size_t nextPos = line.find_first_not_of(" \t", wordEnd);
        bool alone = nextPos == std::string::npos || line.compare(nextPos, 4, "else") == 0 || line.compare(nextPos, 1, ":") == 0;
        if (!afterThen || alone) kind = TokenKind::Label;
      }
      tokens.push_back({tokenStart, wordEnd, kind});
      afterThen = word == "then" || word == "else";
      jumpTarget = jump || (jumpTarget && kind == TokenKind::Label);  // on x goto a b c
    } else {
      i++;
      jumpTarget = afterThen = false;
    }
  }
  return tokens;
}

}  // namespace vcs
