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
std::vector<Word> wordsOf(const std::string& s) {
  std::vector<Word> out;
  int n = (int)s.size(), i = 0;
  while (i < n) {
    while (i < n && std::isspace((unsigned char)s[i])) i++;
    if (i >= n || s[i] == ';' || (s[i] == '/' && i + 1 < n && s[i + 1] == '/')) break;
    int b = i;
    if (s[i] == '"' || s[i] == '<') {
      char close = s[i] == '"' ? '"' : '>';
      i++;
      while (i < n && s[i] != close) i++;
      if (i < n) i++;
    } else {
      while (i < n && !std::isspace((unsigned char)s[i]) && s[i] != ';') i++;
    }
    std::string t = s.substr(b, i - b);
    for (auto& c : t) c = (char)std::tolower((unsigned char)c);
    out.push_back({b, i, t});
  }
  return out;
}

}  // namespace

bool fileNameInLine(const std::string& line, std::string* name, int* start, int* end) {
  auto w = wordsOf(line);
  // the keyword: first, or second after a label (DASM), or "#" "include"
  size_t k = 0, arg = 0;
  bool found = false;
  // a second word only after a label (it starts the line)
  size_t words = !line.empty() && !std::isspace((unsigned char)line[0]) && line[0] != '#' ? 2 : 1;
  for (; k < w.size() && k < words && !found; k++) {
    const std::string& t = w[k].text;
    if (t == "include" || t == "incbin" || t == "#include" || t == "inline" || t == "incgraphic" ||
        t == ".include" || t == ".incbin")
      found = true, arg = k + 1;
    else if (t == "#" && k + 1 < w.size() && w[k + 1].text == "include")
      found = true, arg = k + 2;
    else if (t == "rem" && k + 1 < w.size() && w[k + 1].text == "vcsgfx")
      found = true, arg = k + 2;
  }
  if (!found || arg >= w.size()) return false;
  int b = w[arg].start, e = w[arg].end;
  if (e - b >= 2 && (line[b] == '"' || line[b] == '<')) b++, e--;  // without the quotes or brackets
  else
    while (e > b && line[e - 1] == ',') e--;
  if (e <= b) return false;
  std::string n = line.substr(b, e - b);
  if (n == "end") return false;  // rem vcsgfx end
  if (name) *name = n;
  if (start) *start = b;
  if (end) *end = e;
  return true;
}

}  // namespace vcs
