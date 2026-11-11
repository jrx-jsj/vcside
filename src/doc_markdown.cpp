// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "doc_markdown.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "i18n.h"

namespace vcs {

namespace {
std::string cell(std::string text) {  // a table cell: no pipes, one line
  for (char& c : text)
    if (c == '|') c = '/';
    else if (c == '\n') c = ' ';
  return text;
}

std::string fence(const std::string& code) {
  std::string body = code;
  while (!body.empty() && body.back() == '\n') body.pop_back();
  return "```asm\n" + body + "\n```\n\n";
}

// The uppercase names (3 to 12 letters, digits, underscores) in a text that are registers of the knowledge base.
void mentionedRegisters(const KnowledgeBase& kb, const std::string& text, const std::string& except, std::vector<std::string>* into) {
  size_t at = 0;
  while (at < text.size()) {
    if (!std::isalnum((unsigned char)text[at])) {
      at++;
      continue;
    }
    size_t end = at;
    while (end < text.size() && (std::isalnum((unsigned char)text[end]) || text[end] == '_')) end++;
    std::string word = text.substr(at, end - at);
    at = end;
    if (word.size() < 3 || word.size() > 12 || word == except) continue;
    if (std::any_of(word.begin(), word.end(), [](char c) { return std::islower((unsigned char)c); })) continue;
    if (kb.reg(word) && std::find(into->begin(), into->end(), word) == into->end()) into->push_back(word);
  }
}

void addTopic(const KnowledgeBase& kb, const std::string& name, std::vector<DocLink>* links, std::set<std::string>* seen) {
  std::string name_upper = upper(name);
  if (const Register* reg = kb.reg(name_upper)) {
    if (seen->insert("R:" + reg->name).second) links->push_back({"R:" + reg->name, reg->name + "  " + reg->summary});
  } else if (const Instruction* instruction = kb.instruction(name_upper)) {
    if (seen->insert("I:" + instruction->mnemonic).second) links->push_back({"I:" + instruction->mnemonic, instruction->mnemonic + "  " + instruction->summary});
  }
}

void addTechnique(const Technique& technique, std::vector<DocLink>* links, std::set<std::string>* seen) {
  if (seen->insert("K:" + technique.id).second) links->push_back({"K:" + technique.id, technique.title});
}
}  // namespace

std::string registerMarkdown(const Register& reg) {
  std::string text = reg.description + "\n\n";
  if (!reg.fields.empty()) {
    text += std::string("### ") + T("Bits") + "\n\n| " + T("Bits") + " | " + T("Name") + " | " + T("Meaning") + " |\n| --- | --- | --- |\n";
    for (auto& field : reg.fields) text += "| `" + cell(field.bits) + "` | " + cell(field.name) + " | " + cell(field.meaning) + " |\n";
    text += "\n";
  }
  if (!reg.timing.empty()) text += std::string("### ") + T("Timing") + "\n\n" + reg.timing + "\n\n";
  if (!reg.issues.empty()) {
    text += std::string("### ") + T("Known issues") + "\n\n";
    for (auto& issue : reg.issues) text += "- " + issue + "\n";
    text += "\n";
  }
  if (!reg.example.empty()) text += std::string("### ") + T("Example") + "\n\n" + fence(reg.example);
  return text;
}

std::string techniqueMarkdown(const Technique& technique) {
  std::string text = technique.summary + "\n\n";
  if (!technique.steps.empty()) {
    text += std::string("### ") + T("Steps") + "\n\n";
    int number = 1;
    for (auto& step : technique.steps) text += std::to_string(number++) + ". " + step + "\n";
    text += "\n";
  }
  if (!technique.example.empty()) text += std::string("### ") + T("Example") + "\n\n" + fence(technique.example);
  if (!technique.note.empty()) text += "> " + technique.note + "\n\n";
  return text;
}

std::vector<DocLink> registerSeeAlso(const KnowledgeBase& kb, const Register& reg) {
  std::vector<DocLink> links;
  std::set<std::string> seen;
  seen.insert("R:" + reg.name);
  for (auto& name : reg.seeAlso) {
    bool recipe = false;
    for (auto& technique : kb.techniques())
      if (technique.id == name) addTechnique(technique, &links, &seen), recipe = true;
    if (!recipe) addTopic(kb, name, &links, &seen);
  }
  std::vector<std::string> mentioned;
  mentionedRegisters(kb, reg.summary + " " + reg.description + " " + reg.timing, reg.name, &mentioned);
  for (auto& issue : reg.issues) mentionedRegisters(kb, issue, reg.name, &mentioned);
  for (auto& name : mentioned) addTopic(kb, name, &links, &seen);
  for (auto& technique : kb.techniques())
    for (auto& related : technique.related)
      if (upper(related) == reg.name) addTechnique(technique, &links, &seen);
  if (links.size() > 12) links.resize(12);
  return links;
}

std::vector<DocLink> techniqueSeeAlso(const KnowledgeBase& kb, const Technique& technique) {
  std::vector<DocLink> links;
  std::set<std::string> seen;
  seen.insert("K:" + technique.id);
  for (auto& name : technique.related) addTopic(kb, name, &links, &seen);
  for (auto& other : kb.techniques())
    if (other.id != technique.id)
      for (auto& related : other.related)
        if (std::any_of(technique.related.begin(), technique.related.end(), [&](const std::string& mine) { return upper(mine) == upper(related); })) {
          addTechnique(other, &links, &seen);
          break;
        }
  return links;
}

}  // namespace vcs
