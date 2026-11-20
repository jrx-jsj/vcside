// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "completion_context.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace vcs {

namespace {
std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return text;
}
std::string trimmed(const std::string& text) {
  size_t a = text.find_first_not_of(" \t"), b = text.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
}
}  // namespace

LineContext lineContext(const KnowledgeBase& kb, const std::string& line, const std::string& prefix) {
  LineContext context;
  size_t at = prefix.empty() ? std::string::npos : line.rfind(prefix);
  std::string before = at == std::string::npos ? line : line.substr(0, at);
  size_t comment = before.find(';');
  if (comment != std::string::npos) before = before.substr(0, comment);
  std::istringstream in(before);
  std::vector<std::string> words;
  for (std::string word; in >> word;) words.push_back(word);
  bool labelFirst = !before.empty() && before[0] != ' ' && before[0] != '\t';  // a label stands at the start of the line
  if (labelFirst && !words.empty()) words.erase(words.begin());
  if (words.empty()) return context;
  context.firstWord = false;
  if (kb.instruction(words[0])) context.mnemonic = lowered(words[0]);
  return context;
}

bool registerFits(const Register& reg, const std::string& mnemonic) {
  static const std::set<std::string> stores = {"sta", "stx", "sty", "sax"};
  static const std::set<std::string> reads = {"lda", "ldx", "ldy", "bit", "cmp", "cpx", "cpy", "and", "ora", "eor", "adc", "sbc", "lax"};
  if (stores.count(mnemonic)) return reg.access != "R";
  if (reads.count(mnemonic)) return reg.access != "W";
  return true;  // inc, dec, shifts: read and written; left to the programmer
}

std::string registerDetail(const Register& reg) { return reg.address + " " + reg.access; }

std::vector<RecipeSnippet> recipeSnippets(const KnowledgeBase& kb, const std::string& prefix, const std::string& fileText, const std::string& indent) {
  std::vector<RecipeSnippet> snippets;
  if (prefix.size() < 3) return snippets;
  std::string wanted = lowered(prefix);
  for (auto& technique : kb.techniques()) {
    if (technique.templates.empty() || technique.id.rfind("7800-", 0) == 0 || technique.id.rfind("tinyelf-", 0) == 0) continue;
    bool matches = false;
    std::string id = technique.id;
    for (auto& c : id)
      if (c == '-') c = ' ';
    std::istringstream words(id);
    for (std::string word; words >> word;) matches = matches || word.rfind(wanted, 0) == 0;
    for (auto& keyword : technique.keywords) matches = matches || lowered(keyword).rfind(wanted, 0) == 0;
    if (!matches) continue;
    for (auto& recipe : technique.templates) {
      // a label prefix that is free in the file
      std::string label = technique.id.substr(0, technique.id.find('-'));
      if (!label.empty()) label[0] = (char)std::toupper((unsigned char)label[0]);
      std::string base = label;
      for (int number = 2; fileText.find("." + label) != std::string::npos && number < 50; number++) label = base + std::to_string(number);
      std::string text;
      if (!recipe.variables.empty()) {
        text += "; needs RAM:";
        for (auto& name : recipe.variables) text += " " + name + " ds 1" + (&name == &recipe.variables.back() ? "" : ",");
        text += "\n";
      }
      for (auto& line : recipe.code) {
        std::string code = trimmed(line);
        if (code.size() > 2 && code.front() == '{' && code.back() == '}' && code.find(' ') == std::string::npos && code != "{L}") {
          std::string slot = code.substr(1, code.size() - 2), purpose;
          for (auto& known : recipe.slots)
            if (known.first == slot) purpose = known.second;
          text += indent + "; <" + slot + ": " + purpose + ">\n";
          continue;
        }
        for (size_t at = code.find("{L}"); at != std::string::npos; at = code.find("{L}")) code.replace(at, 3, label);
        for (auto& name : recipe.variables)
          for (size_t at = code.find("{" + name + "}"); at != std::string::npos; at = code.find("{" + name + "}")) code.replace(at, name.size() + 2, name);
        text += (code.rfind(".", 0) == 0 ? code : indent + code) + "\n";
      }
      snippets.push_back({technique.id + "/" + recipe.id, recipe.title, text});
    }
  }
  return snippets;
}

}  // namespace vcs
