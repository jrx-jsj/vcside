// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "cycles.h"

#include <cctype>

namespace vcs {

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static std::string stripComment(const std::string& line) {
  bool quote = false;
  char q = 0;
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (quote) {
      if (c == q) quote = false;
    } else if (c == '"' || (c == '\'' && i + 1 < line.size())) {
      // 'c is a DASM character constant (no closing quote); "..." is a string.
      if (c == '"') { quote = true; q = c; } else i++;
    } else if (c == ';') {
      return line.substr(0, i);
    }
  }
  return line;
}

AsmLine splitAsmLine(const std::string& raw) {
  AsmLine out;
  std::string line = stripComment(raw);
  size_t i = 0;
  auto word = [&]() {
    size_t s = i;
    while (i < line.size() && !std::isspace((unsigned char)line[i])) i++;
    return line.substr(s, i - s);
  };
  auto skip = [&]() { while (i < line.size() && std::isspace((unsigned char)line[i])) i++; };
  if (!line.empty() && !std::isspace((unsigned char)line[0])) {
    out.label = word();
    if (!out.label.empty() && out.label.back() == ':') out.label.pop_back();
  }
  skip();
  out.mnemonic = word();
  skip();
  out.operand = trim(line.substr(std::min(i, line.size())));
  return out;
}

static bool isSymChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.'; }

std::optional<int> evalExpr(const std::string& exprIn, const SymbolLookup& lookup) {
  std::string e = trim(exprIn);
  if (e.empty()) return std::nullopt;
  int sel = 0;  // 1 = low byte, 2 = high byte
  if (e[0] == '<') { sel = 1; e = trim(e.substr(1)); }
  else if (e[0] == '>') { sel = 2; e = trim(e.substr(1)); }
  long total = 0;
  int sign = 1;
  size_t i = 0;
  bool expectTerm = true;
  while (i < e.size()) {
    char c = e[i];
    if (std::isspace((unsigned char)c)) { i++; continue; }
    if (!expectTerm) {
      if (c == '+') sign = 1;
      else if (c == '-') sign = -1;
      else return std::nullopt;  // *, /, parentheses...: not supported
      i++;
      expectTerm = true;
      continue;
    }
    if (c == '-') { sign = -sign; i++; continue; }
    long v = 0;
    if (c == '$') {
      size_t s = ++i;
      while (i < e.size() && std::isxdigit((unsigned char)e[i])) i++;
      if (s == i) return std::nullopt;
      v = std::stol(e.substr(s, i - s), nullptr, 16);
    } else if (c == '%') {
      size_t s = ++i;
      while (i < e.size() && (e[i] == '0' || e[i] == '1')) i++;
      if (s == i) return std::nullopt;
      v = std::stol(e.substr(s, i - s), nullptr, 2);
    } else if (std::isdigit((unsigned char)c)) {
      size_t s = i;
      while (i < e.size() && std::isdigit((unsigned char)e[i])) i++;
      v = std::stol(e.substr(s, i - s));
    } else if (c == '\'' && i + 1 < e.size()) {
      v = (unsigned char)e[i + 1];
      i += 2;
    } else if (isSymChar(c)) {
      size_t s = i;
      while (i < e.size() && isSymChar(e[i])) i++;
      auto r = lookup ? lookup(e.substr(s, i - s)) : std::nullopt;
      if (!r) return std::nullopt;
      v = *r;
    } else {
      return std::nullopt;
    }
    total += sign * v;
    sign = 1;
    expectTerm = false;
  }
  if (expectTerm) return std::nullopt;
  if (sel == 1) total &= 0xFF;
  if (sel == 2) total = (total >> 8) & 0xFF;
  return (int)total;
}

static bool endsWithIndex(const std::string& s, char reg, std::string* base) {
  // "expr,X" (case-insensitive, spaces allowed)
  size_t comma = s.rfind(',');
  if (comma == std::string::npos) return false;
  std::string r = trim(s.substr(comma + 1));
  if (r.size() != 1 || std::toupper((unsigned char)r[0]) != reg) return false;
  *base = trim(s.substr(0, comma));
  return true;
}

LineCycles countLine(const KnowledgeBase& kb, const std::string& line, const SymbolLookup& lookup) {
  LineCycles lc;
  AsmLine a = splitAsmLine(line);
  if (a.mnemonic.empty()) return lc;
  std::string mn = upper(a.mnemonic);
  std::string force;  // "zp" or "abs"
  size_t dot = mn.find('.');
  if (dot != std::string::npos) {
    std::string suf = mn.substr(dot + 1);
    mn = mn.substr(0, dot);
    if (suf == "W" || suf == "A") force = "abs";
    else if (suf == "B" || suf == "Z") force = "zp";
  }

  // Macros from DASM's macro.h whose timing is known.
  if (mn == "SLEEP") {
    lc.isCode = true;
    lc.mnemonic = mn;
    auto n = evalExpr(a.operand, lookup);
    if (n && *n >= 2) {
      lc.known = true;
      lc.min = lc.max = *n;
      lc.bytes = *n / 2;  // approximate: NOPs (1 byte, 2 cycles) plus maybe a NOP zp
    } else {
      lc.error = "SLEEP needs a constant of at least 2";
    }
    return lc;
  }

  const Instruction* in = kb.instruction(mn);
  if (!in) return lc;  // directive, macro or unknown: not code
  lc.isCode = true;
  lc.mnemonic = mn;

  auto has = [&](const char* mode) { return kb.opcode(mn, mode) != nullptr; };
  std::string op = a.operand, base, mode;
  bool guessed = false;

  auto pickWidth = [&](const std::string& expr, const char* zpMode, const char* absMode) {
    if (!force.empty()) return std::string(force == "zp" ? zpMode : absMode);
    auto v = evalExpr(expr, lookup);
    bool zp = v ? (*v >= 0 && *v <= 0xFF) : false;
    if (!v) guessed = true;  // unknown symbol: DASM would decide in a later pass
    if (zp && has(zpMode)) return std::string(zpMode);
    if (has(absMode)) return std::string(absMode);
    return std::string(zpMode);
  };

  if (op.empty()) {
    mode = has("imp") ? "imp" : "acc";
  } else if (upper(op) == "A" && has("acc")) {
    mode = "acc";
  } else if (op[0] == '#') {
    mode = "imm";
  } else if (has("rel")) {
    mode = "rel";
  } else if (op[0] == '(') {
    std::string u = upper(op);
    std::string compact;
    for (char c : u) if (!std::isspace((unsigned char)c)) compact += c;
    if (compact.size() > 3 && compact.compare(compact.size() - 3, 3, ",X)") == 0) mode = "indx";
    else if (compact.size() > 3 && compact.compare(compact.size() - 3, 3, "),Y") == 0) mode = "indy";
    else mode = "ind";
  } else if (endsWithIndex(op, 'X', &base)) {
    mode = pickWidth(base, "zpx", "absx");
  } else if (endsWithIndex(op, 'Y', &base)) {
    mode = pickWidth(base, "zpy", "absy");
  } else if (mn == "JMP" || mn == "JSR") {
    mode = "abs";
  } else {
    mode = pickWidth(op, "zp", "abs");
  }

  const Opcode* o = kb.opcode(mn, mode);
  lc.mode = mode;
  if (!o) {
    lc.error = mn + " has no " + mode + " mode";
    return lc;
  }
  lc.known = true;
  lc.guessed = guessed;
  lc.bytes = o->bytes;
  lc.min = o->cycles;
  lc.max = o->cycles + (o->pagePenalty ? 1 : 0) + (o->branch ? 2 : 0);
  return lc;
}

BlockCycles countBlock(const KnowledgeBase& kb, const std::vector<std::string>& lines, const SymbolLookup& lookup) {
  BlockCycles b;
  for (auto& l : lines) {
    b.lines++;
    LineCycles lc = countLine(kb, l, lookup);
    if (!lc.isCode) continue;
    b.instructions++;
    if (!lc.known) { b.unknown++; continue; }
    if (lc.guessed) b.guessed++;
    b.bytes += lc.bytes;
    b.min += lc.min;
    b.max += lc.max;
  }
  return b;
}

SymbolLookup registerLookup(const KnowledgeBase& kb) {
  return [&kb](const std::string& name) -> std::optional<int> {
    if (const Register* r = kb.reg(name)) {
      int a = r->addr();
      if (a >= 0) return a;
    }
    return std::nullopt;
  };
}

}  // namespace vcs
