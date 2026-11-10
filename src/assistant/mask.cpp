// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/mask.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace vcs::assistant {

namespace {
bool wordChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::string marker(size_t number) { return "[[" + std::to_string(number) + "]]"; }

bool codeLine(const std::string& line, const std::function<bool(const std::string&)>& isMnemonic) {
  size_t first = line.find_first_not_of(" \t");
  if (first == std::string::npos) return false;
  if (line[first] == ';' && first > 0) return true;  // an indented comment
  size_t end = first;
  while (end < line.size() && (wordChar(line[end]) || line[end] == '.')) end++;
  std::string word = line.substr(first, end - first);
  if (word.empty()) return false;
  bool indented = first > 0;
  if (indented && word.size() >= 3 && word.size() <= 4 && isMnemonic(word) && (end == line.size() || line[end] == ' ' || line[end] == '\t' || line[end] == ';')) return true;
  if (indented && word[0] == '.' && word.size() > 2) return true;                                   // .byte, .word
  if (indented && (word == "processor" || word == "include" || word == "org" || word == "seg" || word == "subroutine" || word == "ds" || word == "mac" || word == "endm")) return true;
  // a label alone at the start of a line, or followed by a mnemonic ("Loop lda #1")
  if (!indented && end == line.size() && word.size() >= 3 && std::isupper((unsigned char)word[0]) && word.find(' ') == std::string::npos && word.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos)
    return true;
  return false;
}

bool nameToken(const std::string& token) {
  if (token.size() < 3) return false;
  bool upper = std::all_of(token.begin(), token.end(), [](char c) { return !std::islower((unsigned char)c); }) && std::any_of(token.begin(), token.end(), [](char c) { return std::isupper((unsigned char)c); });
  if (upper) return true;                                                       // SWCHA, VSYNC, INPT4
  if (token.find('_') != std::string::npos) return true;                        // kernel_loop
  if (token.find('/') != std::string::npos) return true;                        // src/main.asm
  size_t dot = token.rfind('.');
  if (dot != std::string::npos && dot > 0 && dot + 1 < token.size() && token.size() - dot - 1 <= 5 && std::all_of(token.begin() + dot + 1, token.end(), [](char c) { return std::isalnum((unsigned char)c); }) &&
      std::any_of(token.begin() + dot + 1, token.end(), [](char c) { return std::isalpha((unsigned char)c); }))
    return true;                                                                // main.asm, hero.vcsgfx
  for (size_t i = 1; i < token.size(); i++)
    if (std::islower((unsigned char)token[i - 1]) && std::isupper((unsigned char)token[i])) return true;  // playerX, PandaControls
  return false;
}
}  // namespace

bool MaskedText::hasProse() const {
  std::string stripped = text;
  for (size_t at = stripped.find("[["); at != std::string::npos; at = stripped.find("[[")) {
    size_t close = stripped.find("]]", at);
    if (close == std::string::npos) break;
    stripped.erase(at, close + 2 - at);
  }
  int letters = 0;
  for (char c : stripped) letters += std::isalpha((unsigned char)c) || (unsigned char)c >= 0x80 ? 1 : 0;
  return letters >= 3;
}

MaskedText maskCode(const std::string& text, const std::function<bool(const std::string&)>& isMnemonic) {
  MaskedText result;
  if (text.find("[[") != std::string::npos) {  // our markers could clash with the text: leave it all as it is
    result.text = text;
    return result;
  }
  auto take = [&](const std::string& span) {
    result.spans.push_back(span);
    return marker(result.spans.size());
  };
  std::string out;
  size_t position = 0;
  // 1. fenced blocks and `inline code`
  std::string stage;
  while (position < text.size()) {
    size_t fence = text.find("```", position);
    size_t tick = text.find('`', position);
    if (tick == std::string::npos) {
      stage += text.substr(position);
      break;
    }
    if (fence == tick) {
      size_t close = text.find("```", fence + 3);
      size_t end = close == std::string::npos ? text.size() : close + 3;
      stage += text.substr(position, fence - position) + take(text.substr(fence, end - fence));
      position = end;
    } else {
      size_t close = text.find('`', tick + 1);
      if (close == std::string::npos) {
        stage += text.substr(position);
        break;
      }
      stage += text.substr(position, tick - position) + take(text.substr(tick, close + 1 - tick));
      position = close + 1;
    }
  }
  // 2. runs of two or more lines of pasted assembly
  std::vector<std::string> lines;
  {
    std::istringstream in(stage);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
  }
  std::string staged;
  for (size_t i = 0; i < lines.size();) {
    size_t run = i;
    while (run < lines.size() && (codeLine(lines[run], isMnemonic) || (run > i && lines[run].find_first_not_of(" \t") == std::string::npos && run + 1 < lines.size() && codeLine(lines[run + 1], isMnemonic)))) run++;
    if (run - i >= 2) {
      std::string block;
      for (size_t k = i; k < run; k++) block += lines[k] + (k + 1 < run ? "\n" : "");
      staged += take(block) + "\n";
      i = run;
    } else {
      staged += lines[i] + "\n";
      i++;
    }
  }
  if (!stage.empty() && stage.back() != '\n' && !staged.empty()) staged.pop_back();
  // 3. names in the prose that is left
  position = 0;
  while (position < staged.size()) {
    if (staged.compare(position, 2, "[[") == 0) {  // a marker already: copy it
      size_t close = staged.find("]]", position);
      out += staged.substr(position, close + 2 - position);
      position = close + 2;
      continue;
    }
    char c = staged[position];
    bool startsToken = wordChar(c) || ((c == '$' || c == '#' || c == '%') && position + 1 < staged.size() && (wordChar(staged[position + 1]) || staged[position + 1] == '$'));
    if (!startsToken) {
      out += c;
      position++;
      continue;
    }
    size_t end = position;
    while (end < staged.size() && (wordChar(staged[end]) || staged[end] == '$' || staged[end] == '#' || staged[end] == '%' || staged[end] == '/' || staged[end] == '.' || staged[end] == '-')) end++;
    while (end > position && (staged[end - 1] == '.' || staged[end - 1] == '-' || staged[end - 1] == '/')) end--;  // trailing punctuation is prose
    std::string token = staged.substr(position, end - position);
    bool hex = (token[0] == '$' || token[0] == '#' || token[0] == '%') && token.size() > 1;
    bool number = std::all_of(token.begin(), token.end(), [](char x) { return std::isdigit((unsigned char)x); });
    if (hex || (!number && nameToken(token))) out += take(token);
    else out += token;
    position = std::max(end, position + 1);
  }
  result.text = out;
  return result;
}

std::string unmaskCode(const std::string& translated, const std::vector<std::string>& spans, bool* ok) {
  *ok = true;
  std::string out;
  std::vector<int> seen(spans.size() + 1, 0);
  size_t position = 0;
  while (position < translated.size()) {
    size_t open = translated.find("[[", position);
    if (open == std::string::npos) {
      out += translated.substr(position);
      break;
    }
    size_t close = translated.find("]]", open);
    out += translated.substr(position, open - position);
    if (close == std::string::npos) {
      *ok = false;
      out += translated.substr(open);
      break;
    }
    std::string number = translated.substr(open + 2, close - open - 2);
    bool digits = !number.empty() && std::all_of(number.begin(), number.end(), [](char c) { return std::isdigit((unsigned char)c); });
    size_t index = digits ? (size_t)std::atoi(number.c_str()) : 0;
    if (!digits || index < 1 || index > spans.size()) {
      *ok = false;
      out += translated.substr(open, close + 2 - open);
    } else {
      seen[index]++;
      out += spans[index - 1];
    }
    position = close + 2;
  }
  for (size_t i = 1; i <= spans.size(); i++)
    if (seen[i] != 1) *ok = false;
  if (!*ok) return translated;
  return out;
}

bool looksEnglish(const std::string& prose) {
  for (char c : prose)
    if ((unsigned char)c >= 0x80) return false;
  static const std::set<std::string> english = {"the", "a", "an", "is", "are", "to", "in", "of", "and", "do", "does", "can", "how", "what", "why", "where", "my", "it", "this", "that", "with", "for", "i", "you", "when", "which", "should"};
  static const std::set<std::string> other = {"el", "la", "los", "las", "que", "como", "para", "un", "una", "en", "de", "del", "con", "por", "se", "es", "lo", "mi", "al", "y", "donde", "cuando", "cual", "le", "les", "des", "est", "une", "pour",
                                              "der", "die", "das", "und", "ist", "ich", "nicht", "il", "che", "di", "non", "um", "uma", "uso", "mais", "pas", "dans"};
  int a = 0, b = 0;
  std::string word;
  auto flush = [&]() {
    if (word.empty()) return;
    std::string lower = word;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    a += english.count(lower) ? 1 : 0;
    b += other.count(lower) ? 1 : 0;
    word.clear();
  };
  for (char c : prose) {
    if (std::isalpha((unsigned char)c)) word += c;
    else flush();
  }
  flush();
  return a >= 1 && a > b;
}

}  // namespace vcs::assistant
