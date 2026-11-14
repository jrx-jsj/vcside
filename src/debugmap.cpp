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
// Any numbered line, also the blank ones (they have no tab after the address).
const std::regex kNumber(R"(^\s*(\d+)\s+[U ]?[0-9a-fA-F]{4,8})");

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
  reserved_.clear();
  bankSize_ = bankSize > 0 ? bankSize : 4096;
  std::istringstream stream(listing);
  std::string text, file, seg;
  std::map<std::string, long> segDelta;
  long delta = 0;
  int lastLine = 0, macroCall = 0;
  // Macro expansions are numbered from the macro, so their lines go to the calling line.
  // Calls on consecutive lines list one expansion after another with nothing between
  // them: when the file goes on n lines after the first call and there were n
  // expansions, each one goes to its own line (otherwise they were nested).
  std::vector<size_t> expansionStarts;  // index in lines_ where each expansion begins
  auto endExpansions = [&](int nextLine) {
    int calls = nextLine - macroCall;
    if (calls > 1 && calls == (int)expansionStarts.size())
      for (int call = 0; call < calls; call++) {
        size_t end = call + 1 < calls ? expansionStarts[call + 1] : lines_.size();
        for (size_t index = expansionStarts[call]; index < end; index++) lines_[index].line = macroCall + call;
      }
    macroCall = 0;
    expansionStarts.clear();
  };
  bool truncatedLast = false;  // the last listed line's bytes go on to the next one
  while (std::getline(stream, text)) {
    if (!text.empty() && text.back() == '\r') text.pop_back();
    std::smatch match;
    if (std::regex_search(text, match, kFile)) {
      file = match[1];
      macroCall = 0;
      expansionStarts.clear();
      continue;
    }
    std::smatch numberMatch;
    if (!std::regex_search(text, numberMatch, kNumber)) continue;
    int num = std::stoi(numberMatch[1]);
    if (num != 0) {
      if (macroCall && num > macroCall) endExpansions(num);  // back in the file after the expansions
      if (!macroCall) lastLine = num;
    }
    if (!std::regex_match(text, match, kLine)) continue;
    bool uninit = match[2] == "U";
    long phys = std::stol(match[3], nullptr, 16);
    std::string rest = match[4];
    int bytes = 0;
    bool truncated = false;  // DASM lists the first bytes and a '*' for the rest
    std::smatch bytesMatch;
    if (std::regex_match(rest, bytesMatch, kBytes)) {
      bytes = (int)words(bytesMatch[1].str()).size();
      size_t bytesEnd = bytesMatch.position(1) + bytesMatch.length(1);
      truncated = bytesEnd < rest.size() && rest[bytesEnd] == '*';
      rest = bytesMatch[2];
    }
    auto wordList = words(rest);
    // Line 0 opens an include (its FILE header follows) or a macro expansion.
    if (num == 0) {
      if (findWord(wordList, "INCLUDE") < 0) {
        if (!macroCall) macroCall = lastLine + 1;
        expansionStarts.push_back(lines_.size());
      }
      continue;
    }
    int line = macroCall ? macroCall : num;

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
    if (uninit) {
      // a variable in RAM (or any space reserved with DS in SEG.U): DASM lists its first
      // bytes as 00 (and an equate's value too, which reserves nothing), so its size comes
      // from DS when it can be worked out, else from what is listed
      int ds = findWord(wordList, "DS") >= 0 ? findWord(wordList, "DS") : findWord(wordList, "DS.B");
      if (ds < 0) continue;
      int size = bytes;
      if (ds + 1 < (int)wordList.size())
        if (auto value = evalExpr(wordList[ds + 1], lookup); value && *value > 0 && *value < 65536) size = (int)*value;
      if (size > 0) reserved_.push_back({file, line, phys, phys + delta, size, rest});
      continue;
    }
    if (bytes == 0) continue;
    if (!lines_.empty() && truncatedLast && phys > lines_.back().phys) lines_.back().bytes = (int)(phys - lines_.back().phys);
    lines_.push_back({file, line, phys, phys + delta, bytes, rest});
    truncatedLast = truncated;
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
  std::vector<ListedLine> keptReserved;
  for (auto& listed : reserved_) {
    int to = listed.file == file ? (listed.line > 0 && listed.line <= (int)lineMap.size() ? lineMap[listed.line - 1] : 0) : listed.line;
    if (to <= 0) continue;
    if (listed.file == file) listed.file = toFile, listed.line = to;
    keptReserved.push_back(listed);
  }
  reserved_ = std::move(keptReserved);
}

void AddressMap::resolveFiles(const std::function<std::string(const std::string&)>& resolve) {
  std::map<std::string, std::string> cache;
  for (auto* list : {&lines_, &reserved_})
  for (auto& listed : *list) {
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

std::vector<const ListedLine*> AddressMap::linesIn(const std::string& file) const {
  std::vector<const ListedLine*> result;
  for (auto& listed : lines_)
    if (listed.file == file) result.push_back(&listed);
  return result;
}

std::vector<const ListedLine*> AddressMap::reservedIn(const std::string& file) const {
  std::vector<const ListedLine*> result;
  for (auto& listed : reserved_)
    if (listed.file == file) result.push_back(&listed);
  return result;
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

std::vector<PageBreak> pageBreaks(const AddressMap& addresses, const std::string& file, const std::vector<std::string>& lines) {
  auto isLabelLine = [&lines](int index) {
    if (index < 0 || index >= (int)lines.size() || lines[index].empty()) return false;
    char first = lines[index][0];
    return !std::isspace((unsigned char)first) && first != ';' && first != '*';
  };
  // ORG, RORG, ALIGN or SEG: they place what follows, so they go below the rule with it
  auto isPlacingLine = [&lines](int index) {
    if (index < 0 || index >= (int)lines.size()) return false;
    std::istringstream words(lines[index]);
    std::string word;
    words >> word;
    if (!lines[index].empty() && !std::isspace((unsigned char)lines[index][0])) words >> word;  // after a label
    if (!word.empty() && word[0] == '.') word.erase(0, 1);
    for (auto& c : word) c = (char)std::toupper((unsigned char)c);
    return word == "ORG" || word == "RORG" || word == "ALIGN" || word == "SEG" || word.rfind("SEG.", 0) == 0;
  };
  auto isBlankLine = [&lines](int index) {
    return index >= 0 && index < (int)lines.size() && lines[index].find_first_not_of(" \t\r") == std::string::npos;
  };
  std::vector<PageBreak> result;
  long lastPage = -1;
  int lastBank = -1, lastLine = 0;  // lastLine: 1-based, as in the listing
  for (const ListedLine* listed : addresses.linesIn(file)) {
    long last = listed->cpu + listed->bytes - 1;
    int bank = addresses.bankOf(*listed);
    if (lastPage >= 0 && ((listed->cpu >> 8) != lastPage || bank != lastBank)) {
      bool newBank = bank != lastBank;
      if (listed->line == lastLine) {
        result.push_back({listed->line, listed->cpu & ~0xFFL, bank, true, newBank});
      } else {
        // above the labels and ORG/ALIGN/SEG lines just before it (blank lines between them included)
        int above = listed->line - 1;
        for (int index = above - 1; index > lastLine - 1; index--) {
          if (isLabelLine(index) || isPlacingLine(index)) above = index;
          else if (!isBlankLine(index)) break;
        }
        result.push_back({above, listed->cpu & ~0xFFL, bank, false, newBank});
      }
    }
    if ((last >> 8) != (listed->cpu >> 8)) result.push_back({listed->line, last & ~0xFFL, bank, true});
    lastPage = last >> 8, lastBank = bank, lastLine = listed->line;
  }
  return result;
}

}  // namespace vcs
