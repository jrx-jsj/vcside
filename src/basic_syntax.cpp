// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "basic_syntax.h"

#include <algorithm>
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

const std::vector<std::string>& tinyBasicKeywords() {
  static const std::vector<std::string> keywords = {
      "PRINT", "LET", "INPUT", "GOTO", "GOSUB", "RETURN", "IF", "FORMAT", "FOR", "TO", "STEP", "NEXT", "STOP", "LIST", "NEW",
      "RUN", "ABS", "RND", "SIZE", "REM", "NOT", "AND", "OR", "LEN", "SGN", "PEEK", "DIM", "CLR", "HIMEM", "TAB", "THEN",
      "END", "POKE", "CONT", "SQR", "POW", "MAP", "DUMP", "BREAK", "SAVE", "LOAD", "GET", "PUT", "SET", "CLS", "LOCATE",
      "ELSE", "DELAY", "MILLIS", "PLAY", "CATALOG", "ENTER", "DELETE", "OPEN", "CLOSE", "USR", "CALL", "SIN", "COS", "TAN",
      "ATAN", "LOG", "EXP", "INT", "DATA", "READ", "RESTORE", "DEF", "FN", "ON", "ERROR", "AFTER", "EVERY", "EVENT",
      "WHILE", "WEND", "REPEAT", "UNTIL", "SWITCH", "CASE", "SWEND", "DO", "DEND", "FEND", "ASC", "CHR", "RIGHT", "LEFT",
      "MID", "SPC", "EDIT", "HELP", "BIT", "GRAPHICS", "STICK", "STRIG", "PTRIG", "PLOT", "DRAWTO", "STR", "INSTR", "VAL"};
  return keywords;
}

std::vector<Token> tokenizeTinyBasic(const std::string& line) {
  std::vector<Token> tokens;
  int length = (int)line.size(), i = 0;
  while (i < length && std::isspace((unsigned char)line[i])) i++;
  if (i < length && std::isdigit((unsigned char)line[i])) {  // the line's number
    int start = i;
    while (i < length && std::isdigit((unsigned char)line[i])) i++;
    tokens.push_back({start, i, TokenKind::Label});
  }
  bool lineTarget = false;  // after GOTO, GOSUB, THEN, ELSE: numbers are lines (ON X GOTO 10,20)
  while (i < length) {
    char character = line[i];
    if (std::isspace((unsigned char)character)) { i++; continue; }
    int start = i;
    if (character == '"') {
      i++;
      while (i < length && line[i] != '"') i++;
      i = std::min(length, i + 1);
      tokens.push_back({start, i, TokenKind::String});
      lineTarget = false;
    } else if (std::isdigit((unsigned char)character) || (character == '.' && i + 1 < length && std::isdigit((unsigned char)line[i + 1]))) {
      while (i < length && (std::isalnum((unsigned char)line[i]) || line[i] == '.')) i++;
      tokens.push_back({start, i, lineTarget ? TokenKind::Label : TokenKind::Number});
    } else if (std::isalpha((unsigned char)character)) {
      while (i < length && std::isalnum((unsigned char)line[i])) i++;
      if (i < length && line[i] == '$') i++;  // CHR$, A$
      std::string word = line.substr(start, i - start);
      for (auto& letter : word) letter = (char)std::toupper((unsigned char)letter);
      const std::string bare = word.back() == '$' ? word.substr(0, word.size() - 1) : word;
      if (bare == "REM") {
        tokens.push_back({start, length, TokenKind::Comment});
        break;
      }
      const auto& keywords = tinyBasicKeywords();
      const bool keyword = std::find(keywords.begin(), keywords.end(), bare) != keywords.end();
      tokens.push_back({start, i, keyword ? TokenKind::Mnemonic : TokenKind::Symbol});
      lineTarget = bare == "GOTO" || bare == "GOSUB" || bare == "THEN" || bare == "ELSE";
    } else {
      i++;
      if (character != ',') lineTarget = false;
    }
  }
  return tokens;
}

}  // namespace vcs
