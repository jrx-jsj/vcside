// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "cycles.h"

#include <cctype>

namespace vcs {

static std::string trim(const std::string& text) {
  size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return {};
  size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(start, end - start + 1);
}

static std::string stripComment(const std::string& line) {
  bool quote = false;
  char quoteChar = 0;
  for (size_t i = 0; i < line.size(); i++) {
    char character = line[i];
    if (quote) {
      if (character == quoteChar) quote = false;
    } else if (character == '"' || (character == '\'' && i + 1 < line.size())) {
      // 'c is a DASM character constant (no closing quote); "..." is a string.
      if (character == '"') { quote = true; quoteChar = character; } else i++;
    } else if (character == ';') {
      return line.substr(0, i);
    }
  }
  return line;
}

AsmLine splitAsmLine(const std::string& raw) {
  AsmLine parsed;
  std::string line = stripComment(raw);
  size_t i = 0;
  auto word = [&]() {
    size_t start = i;
    while (i < line.size() && !std::isspace((unsigned char)line[i])) i++;
    return line.substr(start, i - start);
  };
  auto skip = [&]() { while (i < line.size() && std::isspace((unsigned char)line[i])) i++; };
  if (!line.empty() && !std::isspace((unsigned char)line[0])) {
    parsed.label = word();
    if (!parsed.label.empty() && parsed.label.back() == ':') parsed.label.pop_back();
  }
  skip();
  parsed.mnemonic = word();
  skip();
  parsed.operand = trim(line.substr(std::min(i, line.size())));
  return parsed;
}

std::vector<SourceSymbol> scanAsmSymbols(const std::vector<std::string>& lines) {
  std::vector<SourceSymbol> symbols;
  auto upper = [](std::string text) {
    for (auto& character : text) character = (char)std::toupper((unsigned char)character);
    return text;
  };
  auto valid = [](const std::string& name) {
    if (name.empty() || std::isdigit((unsigned char)name[0])) return false;  // "1$"-style or a number
    for (char character : name)
      if (!std::isalnum((unsigned char)character) && character != '_' && character != '.') return false;
    return name != ".";
  };
  for (int i = 0; i < (int)lines.size(); i++) {
    AsmLine asmLine = splitAsmLine(lines[i]);
    // "N=5" and "N =5" have no space after the name or the "="
    if (size_t equalsPos = asmLine.label.find('='); equalsPos != std::string::npos) {
      std::string rest = asmLine.label.substr(equalsPos + 1);
      asmLine.operand = rest + (asmLine.mnemonic.empty() ? "" : " " + asmLine.mnemonic) + (asmLine.operand.empty() ? "" : " " + asmLine.operand);
      asmLine.label.resize(equalsPos);
      asmLine.mnemonic = "=";
    } else if (asmLine.mnemonic.size() > 1 && asmLine.mnemonic[0] == '=') {
      asmLine.operand = asmLine.mnemonic.substr(1) + (asmLine.operand.empty() ? "" : " " + asmLine.operand);
      asmLine.mnemonic = "=";
    }
    std::string mnemonic = upper(asmLine.mnemonic);
    if (mnemonic == "MAC" || mnemonic == "MACRO") {
      std::string name = asmLine.operand.substr(0, asmLine.operand.find_first_of(" \t,"));
      if (valid(name)) symbols.push_back({name, SourceSymbol::Macro, "", i});
      continue;
    }
    if (!valid(asmLine.label)) continue;
    if (mnemonic == "=" || mnemonic == "EQU" || mnemonic == "SET" || mnemonic == ".SET" || mnemonic == "EQM")
      symbols.push_back({asmLine.label, SourceSymbol::Constant, asmLine.operand, i});
    else
      symbols.push_back({asmLine.label, SourceSymbol::Label, "", i});
  }
  return symbols;
}

static bool isSymChar(char character) { return std::isalnum((unsigned char)character) || character == '_' || character == '.'; }

std::optional<int> evalExpr(const std::string& exprIn, const SymbolLookup& lookup) {
  std::string expression = trim(exprIn);
  if (expression.empty()) return std::nullopt;
  int byteSelect = 0;  // 1 = low byte, 2 = high byte
  if (expression[0] == '<') { byteSelect = 1; expression = trim(expression.substr(1)); }
  else if (expression[0] == '>') { byteSelect = 2; expression = trim(expression.substr(1)); }
  long total = 0;
  int sign = 1;
  size_t i = 0;
  bool expectTerm = true;
  while (i < expression.size()) {
    char character = expression[i];
    if (std::isspace((unsigned char)character)) { i++; continue; }
    if (!expectTerm) {
      if (character == '+') sign = 1;
      else if (character == '-') sign = -1;
      else return std::nullopt;  // *, /, parentheses...: not supported
      i++;
      expectTerm = true;
      continue;
    }
    if (character == '-') { sign = -sign; i++; continue; }
    long value = 0;
    if (character == '$') {
      size_t start = ++i;
      while (i < expression.size() && std::isxdigit((unsigned char)expression[i])) i++;
      if (start == i) return std::nullopt;
      value = std::stol(expression.substr(start, i - start), nullptr, 16);
    } else if (character == '%') {
      size_t start = ++i;
      while (i < expression.size() && (expression[i] == '0' || expression[i] == '1')) i++;
      if (start == i) return std::nullopt;
      value = std::stol(expression.substr(start, i - start), nullptr, 2);
    } else if (std::isdigit((unsigned char)character)) {
      size_t start = i;
      while (i < expression.size() && std::isdigit((unsigned char)expression[i])) i++;
      value = std::stol(expression.substr(start, i - start));
    } else if (character == '\'' && i + 1 < expression.size()) {
      value = (unsigned char)expression[i + 1];
      i += 2;
    } else if (isSymChar(character)) {
      size_t start = i;
      while (i < expression.size() && isSymChar(expression[i])) i++;
      auto found = lookup ? lookup(expression.substr(start, i - start)) : std::nullopt;
      if (!found) return std::nullopt;
      value = *found;
    } else {
      return std::nullopt;
    }
    total += sign * value;
    sign = 1;
    expectTerm = false;
  }
  if (expectTerm) return std::nullopt;
  if (byteSelect == 1) total &= 0xFF;
  if (byteSelect == 2) total = (total >> 8) & 0xFF;
  return (int)total;
}

static bool endsWithIndex(const std::string& text, char reg, std::string* base) {
  // "expr,X" (case-insensitive, spaces allowed)
  size_t comma = text.rfind(',');
  if (comma == std::string::npos) return false;
  std::string indexText = trim(text.substr(comma + 1));
  if (indexText.size() != 1 || std::toupper((unsigned char)indexText[0]) != reg) return false;
  *base = trim(text.substr(0, comma));
  return true;
}

LineCycles countLine(const KnowledgeBase& kb, const std::string& line, const SymbolLookup& lookup) {
  LineCycles cycles;
  AsmLine asmLine = splitAsmLine(line);
  if (asmLine.mnemonic.empty()) return cycles;
  std::string mnemonic = upper(asmLine.mnemonic);
  std::string force;  // "zp" or "abs"
  size_t dot = mnemonic.find('.');
  if (dot != std::string::npos) {
    std::string suffix = mnemonic.substr(dot + 1);
    mnemonic = mnemonic.substr(0, dot);
    if (suffix == "W" || suffix == "A") force = "abs";
    else if (suffix == "B" || suffix == "Z") force = "zp";
  }

  // Macros from DASM's macro.h whose timing is known.
  if (mnemonic == "SLEEP") {
    cycles.isCode = true;
    cycles.mnemonic = mnemonic;
    auto cycleCount = evalExpr(asmLine.operand, lookup);
    if (cycleCount && *cycleCount >= 2) {
      cycles.known = true;
      cycles.min = cycles.max = *cycleCount;
      cycles.bytes = *cycleCount / 2;  // approximate: NOPs (1 byte, 2 cycles) plus maybe a NOP zp
    } else {
      cycles.error = "SLEEP needs a constant of at least 2";
    }
    return cycles;
  }

  const Instruction* instruction = kb.instruction(mnemonic);
  if (!instruction) return cycles;  // directive, macro or unknown: not code
  cycles.isCode = true;
  cycles.mnemonic = mnemonic;

  auto has = [&](const char* mode) { return kb.opcode(mnemonic, mode) != nullptr; };
  std::string operand = asmLine.operand, base, mode;
  bool guessed = false;

  auto pickWidth = [&](const std::string& expr, const char* zpMode, const char* absMode) {
    if (!force.empty()) return std::string(force == "zp" ? zpMode : absMode);
    auto value = evalExpr(expr, lookup);
    bool inZeroPage = value ? (*value >= 0 && *value <= 0xFF) : false;
    if (!value) guessed = true;  // unknown symbol: DASM would decide in a later pass
    if (inZeroPage && has(zpMode)) return std::string(zpMode);
    if (has(absMode)) return std::string(absMode);
    return std::string(zpMode);
  };

  if (operand.empty()) {
    mode = has("imp") ? "imp" : "acc";
  } else if (upper(operand) == "A" && has("acc")) {
    mode = "acc";
  } else if (operand[0] == '#') {
    mode = "imm";
  } else if (has("rel")) {
    mode = "rel";
  } else if (operand[0] == '(') {
    std::string upperOperand = upper(operand);
    std::string compact;
    for (char character : upperOperand) if (!std::isspace((unsigned char)character)) compact += character;
    if (compact.size() > 3 && compact.compare(compact.size() - 3, 3, ",X)") == 0) mode = "indx";
    else if (compact.size() > 3 && compact.compare(compact.size() - 3, 3, "),Y") == 0) mode = "indy";
    else mode = "ind";
  } else if (endsWithIndex(operand, 'X', &base)) {
    mode = pickWidth(base, "zpx", "absx");
  } else if (endsWithIndex(operand, 'Y', &base)) {
    mode = pickWidth(base, "zpy", "absy");
  } else if (mnemonic == "JMP" || mnemonic == "JSR") {
    mode = "abs";
  } else {
    mode = pickWidth(operand, "zp", "abs");
  }

  const Opcode* opcode = kb.opcode(mnemonic, mode);
  cycles.mode = mode;
  if (!opcode) {
    cycles.error = mnemonic + " has no " + mode + " mode";
    return cycles;
  }
  cycles.known = true;
  cycles.guessed = guessed;
  cycles.bytes = opcode->bytes;
  cycles.min = opcode->cycles;
  cycles.max = opcode->cycles + (opcode->pagePenalty ? 1 : 0) + (opcode->branch ? 2 : 0);
  return cycles;
}

BlockCycles countBlock(const KnowledgeBase& kb, const std::vector<std::string>& lines, const SymbolLookup& lookup) {
  BlockCycles block;
  for (auto& line : lines) {
    block.lines++;
    LineCycles cycles = countLine(kb, line, lookup);
    if (!cycles.isCode) continue;
    block.instructions++;
    if (!cycles.known) { block.unknown++; continue; }
    if (cycles.guessed) block.guessed++;
    block.bytes += cycles.bytes;
    block.min += cycles.min;
    block.max += cycles.max;
  }
  return block;
}

SymbolLookup registerLookup(const KnowledgeBase& kb) {
  return [&kb](const std::string& name) -> std::optional<int> {
    if (const Register* reg = kb.reg(name)) {
      int address = reg->addr();
      if (address >= 0) return address;
    }
    return std::nullopt;
  };
}

}  // namespace vcs
