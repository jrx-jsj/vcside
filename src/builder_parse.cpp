// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "builder_parse.h"

#include "i18n.h"

#include <algorithm>
#include <regex>
#include <sstream>

namespace vcs {

static Diagnostic::Severity severityOf(const std::string& s) {
  if (s.find("warning") != std::string::npos) return Diagnostic::Warning;
  if (s.find("note") != std::string::npos) return Diagnostic::Info;
  return Diagnostic::Error;
}

std::vector<Diagnostic> parseDasmOutput(const std::string& output) {
  std::vector<Diagnostic> out;
  static const std::regex lineRe(R"(^(.+?) \((\d+)\): (fatal error|error|warning|abort)\s*:?\s*(.*)$)");
  static const std::regex unresolvedRe(R"(^(\S+)\s+[0-9a-fA-F]+\s+\?\?\?\?)");
  std::istringstream in(output);
  std::string line;
  bool inUnresolved = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch m;
    if (line.rfind("--- Unresolved Symbol List", 0) == 0) { inUnresolved = true; continue; }
    if (line.rfind("---", 0) == 0) { inUnresolved = false; continue; }
    if (inUnresolved && std::regex_search(line, m, unresolvedRe)) {
      out.push_back({Diagnostic::Error, "", 0, "Unresolved symbol: " + m[1].str()});
      continue;
    }
    if (std::regex_match(line, m, lineRe)) {
      out.push_back({severityOf(m[3]), m[1], std::stoi(m[2]), m[4]});
    } else if (line.rfind("Fatal assembly error", 0) == 0) {
      out.push_back({Diagnostic::Error, "", 0, line});
    }
  }
  return out;
}

std::vector<Diagnostic> parseGccOutput(const std::string& output) {
  std::vector<Diagnostic> out;
  static const std::regex re(R"(^(.+?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note):\s*(.*)$)");
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
    std::smatch m;
    if (std::regex_match(line, m, inFunction)) {
      function = m[1];
      continue;
    }
    if (std::regex_match(line, m, re)) {
      out.push_back({severityOf(m[4]), m[1], std::stoi(m[2]), m[5]});
    } else if (std::regex_match(line, m, ldRef)) {
      out.push_back({Diagnostic::Error, m[1], 0, m[2].str() + (function.empty() ? "" : " (in " + function + ")")});
    } else if (std::regex_match(line, m, noLine)) {
      std::string who = m[1], base = who.substr(who.find_last_of('/') + 1);
      if (line.find("has a LOAD segment with RWX permissions") != std::string::npos) continue;  // every firmware link
      if (base.find('.') != std::string::npos) out.push_back({severityOf(m[2]), who, 0, m[3]});
      else out.push_back({severityOf(m[2]), "", 0, base + ": " + m[3].str()});
    } else if (std::regex_match(line, m, makeStop)) {
      // "make: *** [build/x.bin] Error 1" only repeats an error above; the rest is make's own
      std::string what = m[2];
      if (!std::regex_search(what, std::regex(R"(\] Error \d+$)"))) out.push_back({Diagnostic::Error, "", 0, "make: " + what});
    } else if (line.find("undefined reference to") != std::string::npos || line.find("overflowed by") != std::string::npos ||
               line.find("will not fit in region") != std::string::npos) {
      out.push_back({Diagnostic::Error, "", 0, line.substr(line.find(": ") == std::string::npos ? 0 : line.find(": ") + 2)});
    }
    function.clear();
  }
  return out;
}

std::vector<int> basicLineMap(const std::string& asmText) {
  static const std::regex mark(R"(;;line (\d+);;)");
  std::vector<int> map;
  std::istringstream in(asmText);
  std::string line;
  int current = 0;
  while (std::getline(in, line)) {
    std::smatch m;
    if (line.find(";;line ") != std::string::npos && std::regex_search(line, m, mark)) current = std::stoi(m[1]);
    // the includes and the footer after the program are not lines of the .bas
    else if (line.rfind(" if ECHOFIRST", 0) == 0 || line.rfind("; feel free to modify", 0) == 0) current = 0;
    map.push_back(current);
  }
  return map;
}

std::vector<Diagnostic> parseBasicOutput(const std::string& output, const std::string& basFile, const std::string& asmText) {
  std::vector<Diagnostic> out;
  static const std::regex own(R"(^line (\d+): ?(Error|Warning|error|warning):? *(.*)$)");
  static const std::regex jumpTo(R"(^Unknown Mnemonic '(?:jmp|jsr) \.(\w+)'\.?$)");
  std::string asmName = basFile.substr(basFile.find_last_of("/\\") + 1) + ".asm";
  std::vector<int> map;
  bool mapped = false;
  std::istringstream in(output);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch m;
    if (std::regex_match(line, m, own)) out.push_back({severityOf(m[2]), basFile, std::stoi(m[1]), m[3]});
  }
  for (auto d : parseDasmOutput(output)) {
    std::string name = d.file.substr(d.file.find_last_of("/\\") + 1);
    if (name == asmName && d.line > 0) {
      if (!mapped) map = basicLineMap(asmText), mapped = true;
      int bas = d.line <= (int)map.size() ? map[d.line - 1] : 0;
      if (bas > 0) d.file = basFile, d.line = bas;
      std::smatch m;
      if (std::regex_match(d.message, m, jumpTo)) d.message = F("No label called {0} (goto, gosub or then).", {m[1].str()});
    }
    out.push_back(d);
  }
  // "Fatal assembly error: Source is not resolvable." only repeats the errors above it
  if (out.size() > 1)
    out.erase(std::remove_if(out.begin(), out.end(), [](auto& d) { return d.file.empty() && d.message.rfind("Fatal assembly error", 0) == 0; }),
              out.end());
  if (out.empty() && output.find("Compilation failed.") != std::string::npos)
    out.push_back({Diagnostic::Error, basFile, 0, T("batari Basic could not compile the program (see Output).")});
  return out;
}

std::map<std::string, int> parseDasmSymbols(const std::string& text) {
  std::map<std::string, int> out;
  static const std::regex re(R"(^(\S+)\s+([0-9a-fA-F]+)\s)");
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("---", 0) == 0) continue;
    std::smatch m;
    std::string l = line + " ";
    if (std::regex_search(l, m, re)) {
      try { out[m[1]] = std::stoi(m[2], nullptr, 16); } catch (...) {}
    }
  }
  return out;
}

}  // namespace vcs
