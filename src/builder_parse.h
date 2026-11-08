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

// DASM -s symbol file: "Name  f000  (R )" lines.
std::map<std::string, int> parseDasmSymbols(const std::string& text);

}  // namespace vcs
