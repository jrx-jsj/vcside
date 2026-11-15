// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "c_syntax.h"

#include <cctype>
#include <unordered_set>

namespace vcs {

static bool wordChar(char character) { return std::isalnum((unsigned char)character) || character == '_'; }

static const std::unordered_set<std::string>& controlKeywords() {
  static const std::unordered_set<std::string> words = {
      "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "goto",
      "sizeof", "alignof", "_Alignof", "new", "delete", "try", "catch", "throw", "this", "using", "namespace",
      "template", "typename", "operator", "public", "private", "protected", "friend", "true", "false", "nullptr",
      "NULL", "asm", "__asm__", "__attribute__", "static_assert", "_Static_assert", "constexpr", "noexcept"};
  return words;
}

static const std::unordered_set<std::string>& typeKeywords() {
  static const std::unordered_set<std::string> words = {
      "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "bool", "_Bool",
      "const", "volatile", "static", "extern", "inline", "register", "restrict", "auto", "struct", "union",
      "enum", "typedef", "class", "virtual", "override", "final", "mutable", "explicit",
      "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
      "intptr_t", "uintptr_t", "size_t", "ptrdiff_t"};
  return words;
}

// Scans one line. With tokens, records them; without, it only follows the comment state.
static int scanLine(const KnowledgeBase* kb, const std::string& line, int state, std::vector<Token>* tokens) {
  int length = (int)line.size(), index = 0;
  auto push = [&](int start, int end, TokenKind kind) {
    if (tokens && end > start) tokens->push_back({start, end, kind});
  };
  if (state == CLineBlockComment) {
    size_t close = line.find("*/");
    if (close == std::string::npos) {
      push(0, length, TokenKind::Comment);
      return CLineBlockComment;
    }
    index = (int)close + 2;
    push(0, index, TokenKind::Comment);
    state = CLineCode;
  }
  // a preprocessor line: the directive itself, then the rest is coloured as code (#include <x.h> as a string)
  size_t firstNonBlank = line.find_first_not_of(" \t");
  bool preprocessor = firstNonBlank != std::string::npos && (int)firstNonBlank >= index && line[firstNonBlank] == '#';
  if (preprocessor) {
    int directiveEnd = (int)firstNonBlank + 1;
    while (directiveEnd < length && (line[directiveEnd] == ' ' || line[directiveEnd] == '\t')) directiveEnd++;
    while (directiveEnd < length && wordChar(line[directiveEnd])) directiveEnd++;
    push((int)firstNonBlank, directiveEnd, TokenKind::Directive);
    index = directiveEnd;
    size_t nameStart = line.find_first_not_of(" \t", firstNonBlank + 1);
    if (nameStart != std::string::npos && line.compare(nameStart, 7, "include") == 0) {
      size_t open = line.find('<', index);
      size_t close = open == std::string::npos ? std::string::npos : line.find('>', open);
      if (close != std::string::npos) push((int)open, (int)close + 1, TokenKind::String), index = (int)close + 1;
    }
  }
  while (index < length) {
    char character = line[index];
    int tokenStart = index;
    if (character == '/' && index + 1 < length && line[index + 1] == '/') {
      push(index, length, TokenKind::Comment);
      return CLineCode;
    }
    if (character == '/' && index + 1 < length && line[index + 1] == '*') {
      size_t close = line.find("*/", index + 2);
      if (close == std::string::npos) {
        push(index, length, TokenKind::Comment);
        return CLineBlockComment;
      }
      index = (int)close + 2;
      push(tokenStart, index, TokenKind::Comment);
    } else if (character == '"' || character == '\'') {
      index++;
      while (index < length && line[index] != character) index += line[index] == '\\' ? 2 : 1;
      index = std::min(length, index + 1);
      push(tokenStart, index, TokenKind::String);
    } else if (std::isdigit((unsigned char)character) || (character == '.' && index + 1 < length && std::isdigit((unsigned char)line[index + 1]))) {
      // 42, 0x1F, 0b1010, 3.5f, 100UL, 1'000 (C++)
      index++;
      while (index < length && (wordChar(line[index]) || line[index] == '.' || line[index] == '\'')) index++;
      push(tokenStart, index, TokenKind::Number);
    } else if (std::isalpha((unsigned char)character) || character == '_') {
      while (index < length && wordChar(line[index])) index++;
      if (!tokens) continue;
      std::string word = line.substr(tokenStart, index - tokenStart);
      size_t next = line.find_first_not_of(" \t", index);
      TokenKind kind = TokenKind::Text;
      bool hasLower = false, hasLetter = false;
      for (char letter : word) {
        hasLower = hasLower || std::islower((unsigned char)letter);
        hasLetter = hasLetter || std::isalpha((unsigned char)letter);
      }
      if (controlKeywords().count(word)) kind = TokenKind::Mnemonic;
      else if (typeKeywords().count(word)) kind = TokenKind::Directive;
      else if (!hasLower && kb && kb->reg(word)) kind = TokenKind::Register;  // COLUBK; a variable called swcha is not one
      else if (next != std::string::npos && line[next] == '(') kind = TokenKind::Label;  // a call or a definition
      else if (!hasLower && hasLetter && word.size() > 1) kind = TokenKind::Symbol;  // a macro or a constant, by the usual convention
      if (kind != TokenKind::Text) push(tokenStart, index, kind);
    } else {
      index++;
    }
  }
  return state;
}

int cLineEndState(const std::string& line, int startState) { return scanLine(nullptr, line, startState, nullptr); }

std::vector<Token> tokenizeC(const KnowledgeBase& kb, const std::string& line, int startState) {
  std::vector<Token> tokens;
  scanLine(&kb, line, startState, &tokens);
  return tokens;
}

}  // namespace vcs
