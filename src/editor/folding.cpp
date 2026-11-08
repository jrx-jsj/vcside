// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "folding.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace vcs {

static bool blank(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); });
}

static std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) a++;
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

static std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// The words of a line up to its comment (outside quotes).
static std::vector<std::string> words(const std::string& line, char comment) {
  std::vector<std::string> out;
  std::string w;
  char quote = 0;
  for (char c : line) {
    if (quote) {
      w += c;
      if (c == quote) quote = 0;
      continue;
    }
    if (c == comment) break;
    if (c == '"' || c == '\'') quote = c;
    if (std::isspace((unsigned char)c)) {
      if (!w.empty()) out.push_back(w), w.clear();
    } else {
      w += c;
    }
  }
  if (!w.empty()) out.push_back(w);
  return out;
}

namespace {

struct Builder {
  const std::vector<std::string>& lines;
  std::vector<FoldRegion> out;
  // a region; sections (labels, SEG...) also leave out the comments that end them,
  // which usually introduce what comes next
  void add(int first, int last, bool section, bool (*comment)(const std::string&)) {
    while (last > first && (blank(lines[last]) || (section && comment(lines[last])))) last--;
    if (last > first) out.push_back({first, last});
  }
  // runs of two or more comment lines
  void commentRuns(bool (*comment)(const std::string&)) {
    int n = (int)lines.size();
    for (int i = 0; i < n;) {
      if (!comment(lines[i])) {
        i++;
        continue;
      }
      int j = i;
      while (j + 1 < n && comment(lines[j + 1])) j++;
      if (j > i) out.push_back({i, j});
      i = j + 1;
    }
  }
  // sections that run from each start to the line before the next start of the same kind
  void sections(const std::vector<int>& starts, int end, bool (*comment)(const std::string&)) {
    for (size_t k = 0; k < starts.size(); k++) add(starts[k], k + 1 < starts.size() ? starts[k + 1] - 1 : end, true, comment);
  }
  std::vector<FoldRegion> finish() {
    // one region per first line: the largest
    std::map<int, int> best;
    for (auto& r : out)
      if (r.last > r.first) best[r.first] = std::max(best[r.first], r.last);
    std::vector<FoldRegion> v;
    for (auto& [f, l] : best) v.push_back({f, l});
    return v;
  }
};

bool asmComment(const std::string& s) {
  std::string t = trimmed(s);
  return !t.empty() && t[0] == ';';
}

bool basicComment(const std::string& s) {
  std::string t = lower(trimmed(s));
  return t.rfind("rem", 0) == 0 && (t.size() == 3 || std::isspace((unsigned char)t[3])) && t.rfind("rem vcsgfx", 0) != 0;
}

bool cComment(const std::string& s) { return trimmed(s).rfind("//", 0) == 0; }

std::vector<FoldRegion> asmRegions(const std::vector<std::string>& lines) {
  Builder b{lines, {}};
  int n = (int)lines.size();
  std::vector<std::pair<char, int>> stack;  // M macro, I if, R repeat
  std::vector<int> labels, segs, subs;
  auto close = [&](char kind, int at) {
    for (int k = (int)stack.size() - 1; k >= 0; k--)
      if (stack[k].first == kind) {
        b.add(stack[k].second, at, false, asmComment);
        stack.erase(stack.begin() + k, stack.end());
        return;
      }
  };
  for (int i = 0; i < n; i++) {
    const std::string& line = lines[i];
    if (blank(line) || asmComment(line)) continue;
    auto w = words(line, ';');
    if (w.empty()) continue;
    bool labelled = !std::isspace((unsigned char)line[0]);
    std::string label = labelled ? w[0] : "";
    std::string op = lower(labelled ? (w.size() > 1 ? w[1] : "") : w[0]);
    if (!op.empty() && op[0] == '.') op.erase(0, 1);
    if (op == "mac" || op == "macro") stack.push_back({'M', i});
    else if (op == "endm") close('M', i);
    else if (op == "if" || op == "ifconst" || op == "ifnconst") stack.push_back({'I', i});
    else if (op == "endif" || op == "eif") close('I', i);
    else if (op == "repeat") stack.push_back({'R', i});
    else if (op == "repend") close('R', i);
    if (op == "seg" || op.rfind("seg.", 0) == 0) segs.push_back(i);
    if (op == "subroutine") subs.push_back(i);
    // a global label that starts code or data (not an equate)
    if (labelled && label[0] != '.' && op != "=" && op != "equ" && op != "set" && op != "eqm" && op != "mac" &&
        op != "macro" && label.find('=') == std::string::npos)
      labels.push_back(i);
  }
  // labels and SUBROUTINEs end at the next one or at the next SEG
  auto bounded = [&](const std::vector<int>& starts) {
    for (size_t k = 0; k < starts.size(); k++) {
      int end = k + 1 < starts.size() ? starts[k + 1] - 1 : n - 1;
      auto s = std::upper_bound(segs.begin(), segs.end(), starts[k]);
      if (s != segs.end()) end = std::min(end, *s - 1);
      b.add(starts[k], end, true, asmComment);
    }
  };
  bounded(labels);
  bounded(subs);
  b.sections(segs, n - 1, asmComment);
  b.commentRuns(asmComment);
  return b.finish();
}

std::vector<FoldRegion> basicRegions(const std::vector<std::string>& lines) {
  Builder b{lines, {}};
  int n = (int)lines.size();
  std::vector<int> stack, labels;
  int gfx = -1;
  for (int i = 0; i < n; i++) {
    const std::string& line = lines[i];
    if (blank(line)) continue;
    std::string t = lower(trimmed(line));
    if (t.rfind("rem vcsgfx", 0) == 0) {
      if (t.find(" end", 10) != std::string::npos && trimmed(t.substr(10)) == "end") {
        if (gfx >= 0) b.add(gfx, i, false, basicComment), gfx = -1;
      } else {
        gfx = i;
      }
      continue;
    }
    if (basicComment(line)) continue;
    auto w = words(t, 0);
    std::string first = w.empty() ? "" : w[0];
    if (first == "end") {
      if (!stack.empty()) b.add(stack.back(), i, false, basicComment), stack.pop_back();
      continue;
    }
    bool labelled = !std::isspace((unsigned char)line[0]);
    if (first == "function" || first == "data" || first == "sdata" || first == "asm" ||
        (first.size() > 1 && first.back() == ':' && w.size() == 1)) {
      stack.push_back(i);
    } else if (labelled && !first.empty() && first[0] != ';') {
      labels.push_back(i);
    }
  }
  b.sections(labels, n - 1, basicComment);
  b.commentRuns(basicComment);
  return b.finish();
}

std::vector<FoldRegion> cRegions(const std::vector<std::string>& lines) {
  Builder b{lines, {}};
  int n = (int)lines.size();
  std::vector<int> braces, ifs;
  bool inComment = false;
  int commentStart = 0;
  for (int i = 0; i < n; i++) {
    const std::string& s = lines[i];
    if (!inComment) {
      std::string t = trimmed(s);
      if (!t.empty() && t[0] == '#') {
        std::string d = trimmed(t.substr(1));
        if (d.rfind("if", 0) == 0) ifs.push_back(i);
        else if (d.rfind("endif", 0) == 0 && !ifs.empty()) b.add(ifs.back(), i, false, cComment), ifs.pop_back();
        continue;
      }
    }
    char quote = 0;
    for (size_t k = 0; k < s.size(); k++) {
      char c = s[k], d = k + 1 < s.size() ? s[k + 1] : 0;
      if (inComment) {
        if (c == '*' && d == '/') {
          inComment = false, k++;
          if (i > commentStart) b.add(commentStart, i, false, cComment);
        }
        continue;
      }
      if (quote) {
        if (c == '\\') k++;
        else if (c == quote) quote = 0;
        continue;
      }
      if (c == '/' && d == '/') break;
      if (c == '/' && d == '*') inComment = true, commentStart = i, k++;
      else if (c == '"' || c == '\'') quote = c;
      else if (c == '{') {
        // a brace alone on its line belongs to the line above (the function, the struct...)
        int at = i;
        if (trimmed(s) == "{" && i > 0 && !blank(lines[i - 1])) at = i - 1;
        braces.push_back(at);
      } else if (c == '}' && !braces.empty()) {
        if (i > braces.back()) b.add(braces.back(), i, false, cComment);
        braces.pop_back();
      }
    }
  }
  b.commentRuns(cComment);
  return b.finish();
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
