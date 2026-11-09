// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "builder_parse.h"

#include "i18n.h"

#include <algorithm>
#include <regex>
#include <sstream>

namespace vcs {

static Diagnostic::Severity severityOf(const std::string& text) {
  if (text.find("warning") != std::string::npos) return Diagnostic::Warning;
  if (text.find("note") != std::string::npos) return Diagnostic::Info;
  return Diagnostic::Error;
}

std::vector<Diagnostic> parseDasmOutput(const std::string& output) {
  std::vector<Diagnostic> diagnostics;
  static const std::regex lineRe(R"(^(.+?) \((\d+)\): (fatal error|error|warning|abort)\s*:?\s*(.*)$)");
  static const std::regex unresolvedRe(R"(^(\S+)\s+[0-9a-fA-F]+\s+\?\?\?\?)");
  std::istringstream in(output);
  std::string line;
  bool inUnresolved = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch match;
    if (line.rfind("--- Unresolved Symbol List", 0) == 0) { inUnresolved = true; continue; }
    if (line.rfind("---", 0) == 0) { inUnresolved = false; continue; }
    if (inUnresolved && std::regex_search(line, match, unresolvedRe)) {
      diagnostics.push_back({Diagnostic::Error, "", 0, "Unresolved symbol: " + match[1].str()});
      continue;
    }
    if (std::regex_match(line, match, lineRe)) {
      diagnostics.push_back({severityOf(match[3]), match[1], std::stoi(match[2]), match[4]});
    } else if (line.rfind("Fatal assembly error", 0) == 0) {
      diagnostics.push_back({Diagnostic::Error, "", 0, line});
    }
  }
  return diagnostics;
}

std::vector<Diagnostic> parseGccOutput(const std::string& output) {
  std::vector<Diagnostic> diagnostics;
  static const std::regex pattern(R"(^(.+?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note):\s*(.*)$)");
  // "src/a.c: error: ..." (a file without line, e.g. from a Makefile check) or
  // "arm-none-eabi-gcc: error: ..." / ".../arm-none-eabi-ld: warning: ..." (a tool)
  static const std::regex noLine(R"(^(\S+?):\s*(fatal error|error|warning):\s*(.*)$)");
  // ld: "main.c:(.text+0x1c): undefined reference to `foo'", after "x.o: in function `bar':"
  static const std::regex ldRef(R"(^([^\s:]+):\([^)]*\):\s*(undefined reference to .*)$)");
  static const std::regex inFunction(R"(^.*: in function [`'](.+)':$)");
  static const std::regex makeStop(R"(^make(\[\d+\])?: \*\*\* (.*)$)");
  std::istringstream in(output);
  std::string line, function;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch match;
    if (std::regex_match(line, match, inFunction)) {
      function = match[1];
      continue;
    }
    if (std::regex_match(line, match, pattern)) {
      diagnostics.push_back({severityOf(match[4]), match[1], std::stoi(match[2]), match[5]});
    } else if (std::regex_match(line, match, ldRef)) {
      diagnostics.push_back({Diagnostic::Error, match[1], 0, match[2].str() + (function.empty() ? "" : " (in " + function + ")")});
    } else if (std::regex_match(line, match, noLine)) {
      std::string toolName = match[1], base = toolName.substr(toolName.find_last_of('/') + 1);
      if (line.find("has a LOAD segment with RWX permissions") != std::string::npos) continue;  // every firmware link
      if (base.find('.') != std::string::npos) diagnostics.push_back({severityOf(match[2]), toolName, 0, match[3]});
      else diagnostics.push_back({severityOf(match[2]), "", 0, base + ": " + match[3].str()});
    } else if (std::regex_match(line, match, makeStop)) {
      // "make: *** [build/x.bin] Error 1" only repeats an error above; the rest is make's own
      std::string what = match[2];
      if (!std::regex_search(what, std::regex(R"(\] Error \d+$)"))) diagnostics.push_back({Diagnostic::Error, "", 0, "make: " + what});
    } else if (line.find("undefined reference to") != std::string::npos || line.find("overflowed by") != std::string::npos ||
               line.find("will not fit in region") != std::string::npos) {
      diagnostics.push_back({Diagnostic::Error, "", 0, line.substr(line.find(": ") == std::string::npos ? 0 : line.find(": ") + 2)});
    }
    function.clear();
  }
  return diagnostics;
}

std::vector<int> basicLineMap(const std::string& asmText) {
  static const std::regex mark(R"(;;line (\d+);;)");
  std::vector<int> lineMap;
  std::istringstream in(asmText);
  std::string line;
  int current = 0;
  while (std::getline(in, line)) {
    std::smatch match;
    if (line.find(";;line ") != std::string::npos && std::regex_search(line, match, mark)) current = std::stoi(match[1]);
    // the includes and the footer after the program are not lines of the .bas
    else if (line.rfind(" if ECHOFIRST", 0) == 0 || line.rfind("; feel free to modify", 0) == 0) current = 0;
    lineMap.push_back(current);
  }
  return lineMap;
}

std::vector<Diagnostic> parseBasicOutput(const std::string& output, const std::string& basFile, const std::string& asmText) {
  std::vector<Diagnostic> diagnostics;
  static const std::regex basicError(R"(^line (\d+): ?(Error|Warning|error|warning):? *(.*)$)");
  static const std::regex jumpTo(R"(^Unknown Mnemonic '(?:jmp|jsr) \.(\w+)'\.?$)");
  std::string asmName = basFile.substr(basFile.find_last_of("/\\") + 1) + ".asm";
  std::vector<int> lineMap;
  bool mapped = false;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch match;
    if (std::regex_match(line, match, basicError)) diagnostics.push_back({severityOf(match[2]), basFile, std::stoi(match[1]), match[3]});
  }
  for (auto diagnostic : parseDasmOutput(output)) {
    std::string name = diagnostic.file.substr(diagnostic.file.find_last_of("/\\") + 1);
    if (name == asmName && diagnostic.line > 0) {
      if (!mapped) lineMap = basicLineMap(asmText), mapped = true;
      int basLine = diagnostic.line <= (int)lineMap.size() ? lineMap[diagnostic.line - 1] : 0;
      if (basLine > 0) diagnostic.file = basFile, diagnostic.line = basLine;
      std::smatch match;
      if (std::regex_match(diagnostic.message, match, jumpTo)) diagnostic.message = F("No label called {0} (goto, gosub or then).", {match[1].str()});
    }
    diagnostics.push_back(diagnostic);
  }
  // "Fatal assembly error: Source is not resolvable." only repeats the errors above it
  if (diagnostics.size() > 1)
    diagnostics.erase(std::remove_if(diagnostics.begin(), diagnostics.end(), [](auto& diagnostic) { return diagnostic.file.empty() && diagnostic.message.rfind("Fatal assembly error", 0) == 0; }),
              diagnostics.end());
  if (diagnostics.empty() && output.find("Compilation failed.") != std::string::npos)
    diagnostics.push_back({Diagnostic::Error, basFile, 0, T("batari Basic could not compile the program (see Output).")});
  return diagnostics;
}

std::map<std::string, int> parseDasmSymbols(const std::string& text) {
  std::map<std::string, int> symbols;
  static const std::regex pattern(R"(^(\S+)\s+([0-9a-fA-F]+)\s)");
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("---", 0) == 0) continue;
    std::smatch match;
    std::string padded = line + " ";
    if (std::regex_search(padded, match, pattern)) {
      try { symbols[match[1]] = std::stoi(match[2], nullptr, 16); } catch (...) {}
    }
  }
  return symbols;
}

}  // namespace vcs
