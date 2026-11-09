// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "folding.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace vcs {

static bool blank(const std::string& text) {
  return std::all_of(text.begin(), text.end(), [](unsigned char character) { return std::isspace(character); });
}

static std::string trimmed(const std::string& text) {
  size_t start = 0, end = text.size();
  while (start < end && std::isspace((unsigned char)text[start])) start++;
  while (end > start && std::isspace((unsigned char)text[end - 1])) end--;
  return text.substr(start, end - start);
}

static std::string lower(std::string text) {
  for (auto& character : text) character = (char)std::tolower((unsigned char)character);
  return text;
}

// The words of a line up to its comment (outside quotes).
static std::vector<std::string> words(const std::string& line, char comment) {
  std::vector<std::string> result;
  std::string word;
  char quote = 0;
  for (char character : line) {
    if (quote) {
      word += character;
      if (character == quote) quote = 0;
      continue;
    }
    if (character == comment) break;
    if (character == '"' || character == '\'') quote = character;
    if (std::isspace((unsigned char)character)) {
      if (!word.empty()) result.push_back(word), word.clear();
    } else {
      word += character;
    }
  }
  if (!word.empty()) result.push_back(word);
  return result;
}

namespace {

struct Builder {
  const std::vector<std::string>& lines;
  std::vector<FoldRegion> regions;
  // a region; sections (labels, SEG...) also leave out the comments that end them,
  // which usually introduce what comes next
  void add(int first, int last, bool section, bool (*comment)(const std::string&)) {
    while (last > first && (blank(lines[last]) || (section && comment(lines[last])))) last--;
    if (last > first) regions.push_back({first, last});
  }
  // runs of two or more comment lines
  void commentRuns(bool (*comment)(const std::string&)) {
    int lineCount = (int)lines.size();
    for (int i = 0; i < lineCount;) {
      if (!comment(lines[i])) {
        i++;
        continue;
      }
      int j = i;
      while (j + 1 < lineCount && comment(lines[j + 1])) j++;
      if (j > i) regions.push_back({i, j});
      i = j + 1;
    }
  }
  // sections that run from each start to the line before the next start of the same kind
  void sections(const std::vector<int>& starts, int end, bool (*comment)(const std::string&)) {
    for (size_t index = 0; index < starts.size(); index++) add(starts[index], index + 1 < starts.size() ? starts[index + 1] - 1 : end, true, comment);
  }
  std::vector<FoldRegion> finish() {
    // one region per first line: the largest
    std::map<int, int> best;
    for (auto& region : regions)
      if (region.last > region.first) best[region.first] = std::max(best[region.first], region.last);
    std::vector<FoldRegion> result;
    for (auto& [first, last] : best) result.push_back({first, last});
    return result;
  }
};

bool asmComment(const std::string& line) {
  std::string trimmedLine = trimmed(line);
  return !trimmedLine.empty() && trimmedLine[0] == ';';
}

bool basicComment(const std::string& line) {
  std::string trimmedLine = lower(trimmed(line));
  return trimmedLine.rfind("rem", 0) == 0 && (trimmedLine.size() == 3 || std::isspace((unsigned char)trimmedLine[3])) && trimmedLine.rfind("rem vcsgfx", 0) != 0;
}

bool cComment(const std::string& line) { return trimmed(line).rfind("//", 0) == 0; }

std::vector<FoldRegion> asmRegions(const std::vector<std::string>& lines) {
  Builder builder{lines, {}};
  int lineCount = (int)lines.size();
  std::vector<std::pair<char, int>> stack;  // M macro, I if, R repeat
  std::vector<int> labels, segs, subs;
  auto close = [&](char kind, int endLine) {
    for (int index = (int)stack.size() - 1; index >= 0; index--)
      if (stack[index].first == kind) {
        builder.add(stack[index].second, endLine, false, asmComment);
        stack.erase(stack.begin() + index, stack.end());
        return;
      }
  };
  for (int i = 0; i < lineCount; i++) {
    const std::string& line = lines[i];
    if (blank(line) || asmComment(line)) continue;
    auto lineWords = words(line, ';');
    if (lineWords.empty()) continue;
    bool labelled = !std::isspace((unsigned char)line[0]);
    std::string label = labelled ? lineWords[0] : "";
    std::string operation = lower(labelled ? (lineWords.size() > 1 ? lineWords[1] : "") : lineWords[0]);
    if (!operation.empty() && operation[0] == '.') operation.erase(0, 1);
    if (operation == "mac" || operation == "macro") stack.push_back({'M', i});
    else if (operation == "endm") close('M', i);
    else if (operation == "if" || operation == "ifconst" || operation == "ifnconst") stack.push_back({'I', i});
    else if (operation == "endif" || operation == "eif") close('I', i);
    else if (operation == "repeat") stack.push_back({'R', i});
    else if (operation == "repend") close('R', i);
    if (operation == "seg" || operation.rfind("seg.", 0) == 0) segs.push_back(i);
    if (operation == "subroutine") subs.push_back(i);
    // a global label that starts code or data (not an equate)
    if (labelled && label[0] != '.' && operation != "=" && operation != "equ" && operation != "set" && operation != "eqm" && operation != "mac" &&
        operation != "macro" && label.find('=') == std::string::npos)
      labels.push_back(i);
  }
  // labels and SUBROUTINEs end at the next one or at the next SEG
  auto bounded = [&](const std::vector<int>& starts) {
    for (size_t index = 0; index < starts.size(); index++) {
      int end = index + 1 < starts.size() ? starts[index + 1] - 1 : lineCount - 1;
      auto nextSeg = std::upper_bound(segs.begin(), segs.end(), starts[index]);
      if (nextSeg != segs.end()) end = std::min(end, *nextSeg - 1);
      builder.add(starts[index], end, true, asmComment);
    }
  };
  bounded(labels);
  bounded(subs);
  builder.sections(segs, lineCount - 1, asmComment);
  builder.commentRuns(asmComment);
  return builder.finish();
}

std::vector<FoldRegion> basicRegions(const std::vector<std::string>& lines) {
  Builder builder{lines, {}};
  int lineCount = (int)lines.size();
  std::vector<int> stack, labels;
  int gfxStart = -1;
  for (int i = 0; i < lineCount; i++) {
    const std::string& line = lines[i];
    if (blank(line)) continue;
    std::string lowered = lower(trimmed(line));
    if (lowered.rfind("rem vcsgfx", 0) == 0) {
      if (lowered.find(" end", 10) != std::string::npos && trimmed(lowered.substr(10)) == "end") {
        if (gfxStart >= 0) builder.add(gfxStart, i, false, basicComment), gfxStart = -1;
      } else {
        gfxStart = i;
      }
      continue;
    }
    if (basicComment(line)) continue;
    auto lineWords = words(lowered, 0);
    std::string first = lineWords.empty() ? "" : lineWords[0];
    if (first == "end") {
      if (!stack.empty()) builder.add(stack.back(), i, false, basicComment), stack.pop_back();
      continue;
    }
    bool labelled = !std::isspace((unsigned char)line[0]);
    if (first == "function" || first == "data" || first == "sdata" || first == "asm" ||
        (first.size() > 1 && first.back() == ':' && lineWords.size() == 1)) {
      stack.push_back(i);
    } else if (labelled && !first.empty() && first[0] != ';') {
      labels.push_back(i);
    }
  }
  builder.sections(labels, lineCount - 1, basicComment);
  builder.commentRuns(basicComment);
  return builder.finish();
}

std::vector<FoldRegion> cRegions(const std::vector<std::string>& lines) {
  Builder builder{lines, {}};
  int lineCount = (int)lines.size();
  std::vector<int> braces, ifs;
  bool inComment = false;
  int commentStart = 0;
  for (int i = 0; i < lineCount; i++) {
    const std::string& lineText = lines[i];
    if (!inComment) {
      std::string trimmedLine = trimmed(lineText);
      if (!trimmedLine.empty() && trimmedLine[0] == '#') {
        std::string directive = trimmed(trimmedLine.substr(1));
        if (directive.rfind("if", 0) == 0) ifs.push_back(i);
        else if (directive.rfind("endif", 0) == 0 && !ifs.empty()) builder.add(ifs.back(), i, false, cComment), ifs.pop_back();
        continue;
      }
    }
    char quote = 0;
    for (size_t column = 0; column < lineText.size(); column++) {
      char character = lineText[column], next = column + 1 < lineText.size() ? lineText[column + 1] : 0;
      if (inComment) {
        if (character == '*' && next == '/') {
          inComment = false, column++;
          if (i > commentStart) builder.add(commentStart, i, false, cComment);
        }
        continue;
      }
      if (quote) {
        if (character == '\\') column++;
        else if (character == quote) quote = 0;
        continue;
      }
      if (character == '/' && next == '/') break;
      if (character == '/' && next == '*') inComment = true, commentStart = i, column++;
      else if (character == '"' || character == '\'') quote = character;
      else if (character == '{') {
        // a brace alone on its line belongs to the line above (the function, the struct...)
        int braceLine = i;
        if (trimmed(lineText) == "{" && i > 0 && !blank(lines[i - 1])) braceLine = i - 1;
        braces.push_back(braceLine);
      } else if (character == '}' && !braces.empty()) {
        if (i > braces.back()) builder.add(braces.back(), i, false, cComment);
        braces.pop_back();
      }
    }
  }
  builder.commentRuns(cComment);
  return builder.finish();
}

}  // namespace

std::vector<FoldRegion> foldRegions(const std::vector<std::string>& lines, FoldSyntax syntax) {
  switch (syntax) {
    case FoldSyntax::Asm: return asmRegions(lines);
    case FoldSyntax::Basic: return basicRegions(lines);
    case FoldSyntax::C: return cRegions(lines);
    default: return {};
  }
}

}  // namespace vcs
