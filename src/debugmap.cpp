// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "debugmap.h"

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>

namespace vcs {

namespace {
// "     12  f000\t\t       a9 01\t   B0\t      lda\t#1"  (bytes are optional)
const std::regex kLine(R"(^\s*(\d+)\s([U ])([0-9a-fA-F]{4,8})(?: \?\?\?\?)?\t(.*)$)");
const std::regex kBytes(R"(^\t {7}((?:[0-9a-f]{2} ?)+)\*?(.*)$)");
const std::regex kFile(R"(^------- FILE (\S+))");

std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> result;
  std::string current;
  for (char character : text) {
    if (character == ';') break;
    if (std::isspace((unsigned char)character)) {
      if (!current.empty()) result.push_back(current), current.clear();
    } else {
      current += character;
    }
  }
  if (!current.empty()) result.push_back(current);
  return result;
}

int findWord(const std::vector<std::string>& wordList, const char* name) {
  for (size_t i = 0; i < wordList.size() && i < 3; i++)
    if (upper(wordList[i]) == name) return (int)i;
  return -1;
}
}  // namespace

void AddressMap::load(const std::string& listing, const SymbolLookup& lookup, long bankSize) {
  lines_.clear();
  bankSize_ = bankSize > 0 ? bankSize : 4096;
  std::istringstream stream(listing);
  std::string text, file, seg;
  std::map<std::string, long> segDelta;
  long delta = 0;
  int lastLine = 0, macroCall = 0;
  while (std::getline(stream, text)) {
    if (!text.empty() && text.back() == '\r') text.pop_back();
    std::smatch match;
    if (std::regex_search(text, match, kFile)) {
      file = match[1];
      macroCall = 0;
      continue;
    }
    if (!std::regex_match(text, match, kLine)) continue;
    int num = std::stoi(match[1]);
    bool uninit = match[2] == "U";
    long phys = std::stol(match[3], nullptr, 16);
    std::string rest = match[4];
    int bytes = 0;
    std::smatch bytesMatch;
    if (std::regex_match(rest, bytesMatch, kBytes)) {
      bytes = (int)words(bytesMatch[1].str()).size();
      rest = bytesMatch[2];
    }
    auto wordList = words(rest);
    // Line 0 opens an include (its FILE header follows) or a macro expansion,
    // whose lines are numbered from the macro: give them to the calling line.
    if (num == 0) {
      if (findWord(wordList, "INCLUDE") < 0) macroCall = lastLine + 1;
      continue;
    }
    if (macroCall && num == macroCall + 1) macroCall = 0;  // back in the file after the expansion
    int line = macroCall ? macroCall : num;
    if (!macroCall) lastLine = num;

    int segAt = findWord(wordList, "SEG") >= 0 ? findWord(wordList, "SEG") : findWord(wordList, "SEG.U");
    if (int i = segAt; i >= 0) {
      segDelta[seg] = delta;
      seg = i + 1 < (int)wordList.size() ? wordList[i + 1] : "";
      delta = segDelta.count(seg) ? segDelta[seg] : 0;
    }
    if (int i = findWord(wordList, "RORG"); i >= 0 && i + 1 < (int)wordList.size()) {
      if (auto value = evalExpr(wordList[i + 1], lookup)) delta = *value - phys;
    }
    if (findWord(wordList, "REND") >= 0) delta = 0;
    if (uninit || bytes == 0) continue;
    lines_.push_back({file, line, phys, phys + delta, bytes, rest});
  }
  if (lines_.empty()) return;
  long lowest = lines_[0].phys, highest = 0;
  for (auto& listed : lines_) lowest = std::min(lowest, listed.phys), highest = std::max(highest, listed.phys + listed.bytes);
  base_ = lowest - lowest % bankSize_;
  banks_ = std::max(1, (int)((highest - base_ + bankSize_ - 1) / bankSize_));
}

void AddressMap::remap(const std::string& file, const std::vector<int>& lineMap, const std::string& toFile) {
  std::vector<ListedLine> kept;
  for (auto& listed : lines_) {
    if (listed.file != file) {
      kept.push_back(listed);
      continue;
    }
    int to = listed.line > 0 && listed.line <= (int)lineMap.size() ? lineMap[listed.line - 1] : 0;
    if (to <= 0) continue;
    listed.file = toFile;
    listed.line = to;
    kept.push_back(listed);
  }
  lines_ = std::move(kept);
}

void AddressMap::resolveFiles(const std::function<std::string(const std::string&)>& resolve) {
  std::map<std::string, std::string> cache;
  for (auto& listed : lines_) {
    auto found = cache.find(listed.file);
    if (found == cache.end()) found = cache.emplace(listed.file, resolve(listed.file)).first;
    listed.file = found->second;
  }
}

std::optional<Breakpoint> AddressMap::breakpointAt(const std::string& file, int line,
                                                   const std::function<bool(const ListedLine&)>& isCode) const {
  const ListedLine* best = nullptr;
  for (auto& listed : lines_) {
    if (listed.file != file || listed.line < line || !isCode(listed)) continue;
    if (!best || listed.line < best->line) best = &listed;
  }
  if (!best) return std::nullopt;
  return Breakpoint{best->cpu, bankOf(*best), best};
}

std::vector<const ListedLine*> AddressMap::linesAt(long cpu, int bank) const {
  std::vector<const ListedLine*> result;
  for (auto& listed : lines_) {
    bool hit = cpu >= listed.cpu && cpu < listed.cpu + listed.bytes;
    // the 6507 has 13 address lines: $F000, $1000, $3000... are the same place
    if (!hit) hit = (cpu & 0x1FFF) >= (listed.cpu & 0x1FFF) && (cpu & 0x1FFF) < (listed.cpu & 0x1FFF) + listed.bytes;
    if (hit && (bank < 0 || bankOf(listed) == bank)) result.push_back(&listed);
  }
  return result;
}

}  // namespace vcs
