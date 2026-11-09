// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "source_links.h"

#include <cctype>
#include <vector>

namespace vcs {

namespace {

struct Word {
  int start, end;
  std::string text;  // lower case
};

// The words of the line up to a comment (; or //), with their positions.
std::vector<Word> wordsOf(const std::string& text) {
  std::vector<Word> words;
  int length = (int)text.size(), i = 0;
  while (i < length) {
    while (i < length && std::isspace((unsigned char)text[i])) i++;
    if (i >= length || text[i] == ';' || (text[i] == '/' && i + 1 < length && text[i + 1] == '/')) break;
    int begin = i;
    if (text[i] == '"' || text[i] == '<') {
      char close = text[i] == '"' ? '"' : '>';
      i++;
      while (i < length && text[i] != close) i++;
      if (i < length) i++;
    } else {
      while (i < length && !std::isspace((unsigned char)text[i]) && text[i] != ';') i++;
    }
    std::string word = text.substr(begin, i - begin);
    for (auto& character : word) character = (char)std::tolower((unsigned char)character);
    words.push_back({begin, i, word});
  }
  return words;
}

}  // namespace

bool fileNameInLine(const std::string& line, std::string* name, int* start, int* end) {
  auto lineWords = wordsOf(line);
  // the keyword: first, or second after a label (DASM), or "#" "include"
  size_t wordIndex = 0, argumentIndex = 0;
  bool found = false;
  // a second word only after a label (it starts the line)
  size_t words = !line.empty() && !std::isspace((unsigned char)line[0]) && line[0] != '#' ? 2 : 1;
  for (; wordIndex < lineWords.size() && wordIndex < words && !found; wordIndex++) {
    const std::string& word = lineWords[wordIndex].text;
    if (word == "include" || word == "incbin" || word == "#include" || word == "inline" || word == "incgraphic" ||
        word == ".include" || word == ".incbin")
      found = true, argumentIndex = wordIndex + 1;
    else if (word == "#" && wordIndex + 1 < lineWords.size() && lineWords[wordIndex + 1].text == "include")
      found = true, argumentIndex = wordIndex + 2;
    else if (word == "rem" && wordIndex + 1 < lineWords.size() && lineWords[wordIndex + 1].text == "vcsgfx")
      found = true, argumentIndex = wordIndex + 2;
  }
  if (!found || argumentIndex >= lineWords.size()) return false;
  int from = lineWords[argumentIndex].start, to = lineWords[argumentIndex].end;
  if (to - from >= 2 && (line[from] == '"' || line[from] == '<')) from++, to--;  // without the quotes or brackets
  else
    while (to > from && line[to - 1] == ',') to--;
  if (to <= from) return false;
  std::string fileName = line.substr(from, to - from);
  if (fileName == "end") return false;  // rem vcsgfx end
  if (name) *name = fileName;
  if (start) *start = from;
  if (end) *end = to;
  return true;
}

}  // namespace vcs
