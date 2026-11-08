// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "basic_syntax.h"

#include <cctype>

namespace vcs {

static bool wordChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::string basicLabelOf(const std::string& line) {
  if (line.empty() || !wordChar(line[0])) return {};
  size_t e = 0;
  while (e < line.size() && wordChar(line[e])) e++;
  if (e < line.size() && line[e] == ':') return {};  // a block such as player0: or playfield:
  return line.substr(0, e);
}

std::vector<Token> tokenizeBasic(const KnowledgeBase& kb, const std::string& l) {
  std::vector<Token> t;
  int n = (int)l.size(), i = 0;
  bool jumpTarget = false;  // after goto, gosub, then: a bare word is a label
  bool afterThen = false;
  if (!basicLabelOf(l).empty()) {
    while (i < n && wordChar(l[i])) i++;
    t.push_back({0, i, TokenKind::Label});
  }
  while (i < n) {
    char c = l[i];
    if (std::isspace((unsigned char)c)) { i++; continue; }
    int s = i;
    if (c == '"') {
      i++;
      while (i < n && l[i] != '"') i++;
      i = std::min(n, i + 1);
      t.push_back({s, i, TokenKind::String});
    } else if (c == '$' || c == '%' || std::isdigit((unsigned char)c)) {
      i++;
      while (i < n && (wordChar(l[i]) || l[i] == '.')) i++;  // also 8kSC
      t.push_back({s, i, TokenKind::Number});
      jumpTarget = jumpTarget && std::isdigit((unsigned char)c);  // goto 100
      afterThen = false;
    } else if (std::isalpha((unsigned char)c) || c == '_') {
      while (i < n && wordChar(l[i])) i++;
      std::string w = l.substr(s, i - s);
      if (w == "rem") {
        t.push_back({s, n, TokenKind::Comment});
        break;
      }
      int e = i;
      if (i < n && l[i] == ':' && kb.basic(w + ":")) w += ":", e = ++i;
      TokenKind k = TokenKind::Symbol;
      bool jump = false;
      if (const BasicKeyword* b = kb.basic(w)) {
        if (b->kind == "variable") k = TokenKind::Register;
        else if (b->kind == "directive" || b->kind == "block") k = TokenKind::Directive;
        else k = TokenKind::Mnemonic;
        jump = w == "goto" || w == "gosub" || w == "then" || w == "else";
      } else if (kb.reg(w)) {
        k = TokenKind::Register;
      } else if (jumpTarget) {
        // after then, only a word alone is a label (then a = 1 is a statement)
        size_t r = l.find_first_not_of(" \t", e);
        bool alone = r == std::string::npos || l.compare(r, 4, "else") == 0 || l.compare(r, 1, ":") == 0;
        if (!afterThen || alone) k = TokenKind::Label;
      }
      t.push_back({s, e, k});
      afterThen = w == "then" || w == "else";
      jumpTarget = jump || (jumpTarget && k == TokenKind::Label);  // on x goto a b c
    } else {
      i++;
      jumpTarget = afterThen = false;
    }
  }
  return t;
}

}  // namespace vcs
