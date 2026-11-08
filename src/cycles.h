// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Cycle counting for DASM-syntax 6502 source.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kb.h"

namespace vcs {

// Resolves a symbol (label, equate, register) to its value, if known.
using SymbolLookup = std::function<std::optional<int>(const std::string&)>;

struct LineCycles {
  bool isCode = false;     // the line holds an instruction (or a cycle-known macro)
  bool known = false;      // the instruction and addressing mode were recognised
  bool guessed = false;    // zero page vs absolute was guessed (unknown symbol)
  std::string mnemonic, mode;
  int bytes = 0;
  int min = 0, max = 0;    // cycles: min = no page crossing / branch not taken
  std::string error;
};

struct BlockCycles {
  int lines = 0, instructions = 0, unknown = 0, guessed = 0;
  int bytes = 0, min = 0, max = 0;
};

// Splits a line into label / mnemonic / operand (comments removed).
struct AsmLine {
  std::string label, mnemonic, operand;
};
AsmLine splitAsmLine(const std::string& line);

// Evaluates a simple DASM expression: numbers ($hex, %bin, decimal, 'c'),
// symbols, + - and the < > byte selectors. Returns nullopt if a symbol is unknown.
std::optional<int> evalExpr(const std::string& expr, const SymbolLookup& lookup);

LineCycles countLine(const KnowledgeBase& kb, const std::string& line, const SymbolLookup& lookup);
BlockCycles countBlock(const KnowledgeBase& kb, const std::vector<std::string>& lines, const SymbolLookup& lookup);

// Default lookup: TIA/RIOT register names from the knowledge base.
SymbolLookup registerLookup(const KnowledgeBase& kb);

}  // namespace vcs
