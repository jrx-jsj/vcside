// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// Parsing of tool output: compiler diagnostics and symbol files.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace vcs {

struct Diagnostic {
  enum Severity { Error, Warning, Info } severity = Error;
  std::string file;  // as printed by the tool
  int line = 0;      // 1-based, 0 = no line
  std::string message;
};

// DASM: "file.asm (12): error: Unknown Mnemonic 'foo'." and the unresolved symbol list.
std::vector<Diagnostic> parseDasmOutput(const std::string& output);

// GCC/Clang style: "file.c:12:5: error: message"
std::vector<Diagnostic> parseGccOutput(const std::string& output);

// batari Basic: its own "line 4: Error: ..." (lines of the .bas), DASM's errors in the
// generated <main>.asm taken back to the .bas line through the ";;line N;;" marks it
// writes, and "Compilation failed." basFile is the .bas as the project names it.
std::vector<Diagnostic> parseBasicOutput(const std::string& output, const std::string& basFile, const std::string& asmText);
// For each line of the generated .asm (index = line - 1), the .bas line it comes from (0 = none).
std::vector<int> basicLineMap(const std::string& asmText);

// DASM -s symbol file: "Name  f000  (R )" lines.
std::map<std::string, int> parseDasmSymbols(const std::string& text);

}  // namespace vcs
