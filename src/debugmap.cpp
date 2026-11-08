// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
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

std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> w;
  std::string cur;
  for (char c : s) {
    if (c == ';') break;
    if (std::isspace((unsigned char)c)) {
      if (!cur.empty()) w.push_back(cur), cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) w.push_back(cur);
  return w;
}

int findWord(const std::vector<std::string>& w, const char* name) {
  for (size_t i = 0; i < w.size() && i < 3; i++)
    if (upper(w[i]) == name) return (int)i;
  return -1;
}
}  // namespace

void AddressMap::load(const std::string& listing, const SymbolLookup& lookup, long bankSize) {
  lines_.clear();
  bankSize_ = bankSize > 0 ? bankSize : 4096;
  std::istringstream in(listing);
  std::string text, file, seg;
  std::map<std::string, long> segDelta;
  long delta = 0;
  int lastLine = 0, macroCall = 0;
  while (std::getline(in, text)) {
    if (!text.empty() && text.back() == '\r') text.pop_back();
    std::smatch m;
    if (std::regex_search(text, m, kFile)) {
      file = m[1];
      macroCall = 0;
      continue;
    }
    if (!std::regex_match(text, m, kLine)) continue;
    int num = std::stoi(m[1]);
    bool uninit = m[2] == "U";
    long phys = std::stol(m[3], nullptr, 16);
    std::string rest = m[4];
    int bytes = 0;
    std::smatch b;
    if (std::regex_match(rest, b, kBytes)) {
      bytes = (int)words(b[1].str()).size();
      rest = b[2];
    }
    auto w = words(rest);
    // Line 0 opens an include (its FILE header follows) or a macro expansion,
    // whose lines are numbered from the macro: give them to the calling line.
    if (num == 0) {
      if (findWord(w, "INCLUDE") < 0) macroCall = lastLine + 1;
      continue;
    }
    if (macroCall && num == macroCall + 1) macroCall = 0;  // back in the file after the expansion
    int line = macroCall ? macroCall : num;
    if (!macroCall) lastLine = num;

    int segAt = findWord(w, "SEG") >= 0 ? findWord(w, "SEG") : findWord(w, "SEG.U");
    if (int i = segAt; i >= 0) {
      segDelta[seg] = delta;
      seg = i + 1 < (int)w.size() ? w[i + 1] : "";
      delta = segDelta.count(seg) ? segDelta[seg] : 0;
    }
    if (int i = findWord(w, "RORG"); i >= 0 && i + 1 < (int)w.size()) {
      if (auto v = evalExpr(w[i + 1], lookup)) delta = *v - phys;
    }
    if (findWord(w, "REND") >= 0) delta = 0;
    if (uninit || bytes == 0) continue;
    lines_.push_back({file, line, phys, phys + delta, bytes, rest});
  }
  if (lines_.empty()) return;
  long lo = lines_[0].phys, hi = 0;
  for (auto& l : lines_) lo = std::min(lo, l.phys), hi = std::max(hi, l.phys + l.bytes);
  base_ = lo - lo % bankSize_;
  banks_ = std::max(1, (int)((hi - base_ + bankSize_ - 1) / bankSize_));
}

void AddressMap::remap(const std::string& file, const std::vector<int>& lineMap, const std::string& toFile) {
  std::vector<ListedLine> kept;
  for (auto& l : lines_) {
    if (l.file != file) {
      kept.push_back(l);
      continue;
    }
    int to = l.line > 0 && l.line <= (int)lineMap.size() ? lineMap[l.line - 1] : 0;
    if (to <= 0) continue;
    l.file = toFile;
    l.line = to;
    kept.push_back(l);
  }
  lines_ = std::move(kept);
}

void AddressMap::resolveFiles(const std::function<std::string(const std::string&)>& resolve) {
  std::map<std::string, std::string> cache;
  for (auto& l : lines_) {
    auto it = cache.find(l.file);
    if (it == cache.end()) it = cache.emplace(l.file, resolve(l.file)).first;
    l.file = it->second;
  }
}

std::optional<Breakpoint> AddressMap::breakpointAt(const std::string& file, int line,
                                                   const std::function<bool(const ListedLine&)>& isCode) const {
  const ListedLine* best = nullptr;
  for (auto& l : lines_) {
    if (l.file != file || l.line < line || !isCode(l)) continue;
    if (!best || l.line < best->line) best = &l;
  }
  if (!best) return std::nullopt;
  return Breakpoint{best->cpu, bankOf(*best), best};
}

std::vector<const ListedLine*> AddressMap::linesAt(long cpu, int bank) const {
  std::vector<const ListedLine*> out;
  for (auto& l : lines_) {
    bool hit = cpu >= l.cpu && cpu < l.cpu + l.bytes;
    // the 6507 has 13 address lines: $F000, $1000, $3000... are the same place
    if (!hit) hit = (cpu & 0x1FFF) >= (l.cpu & 0x1FFF) && (cpu & 0x1FFF) < (l.cpu & 0x1FFF) + l.bytes;
    if (hit && (bank < 0 || bankOf(l) == bank)) out.push_back(&l);
  }
  return out;
}

}  // namespace vcs
