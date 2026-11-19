// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/guided.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

#include "assistant/domain_checks.h"
#include "assistant/prompt.h"

namespace vcs::assistant {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

namespace {

const char* kSystem =
    "You help make ONE change to an Atari 2600 game written in 6502 assembly for the DASM assembler. VCSIDE, the editor, leads: it asks you one small question at a time and you answer "
    "ONLY with the JSON object it asks for. The 2600 has a 6507 (a 6502: no 65C02 instructions such as bra, stz, phx), 128 bytes of RAM and the TIA and RIOT registers. "
    "Use only names that exist: the registers of the reference, the names of the file you are shown, and the new variables you declare.";

std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return text;
}

std::string trimmed(const std::string& line) {
  size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
  return a == std::string::npos ? std::string() : line.substr(a, b - a + 1);
}

std::string codeOf(const std::string& line) {
  size_t semicolon = line.find(';');
  return semicolon == std::string::npos ? line : line.substr(0, semicolon);
}

std::vector<std::string> wordsOf(const std::string& text) {
  std::vector<std::string> words;
  std::istringstream in(text);
  for (std::string word; in >> word;) words.push_back(word);
  return words;
}

bool isIdentifier(const std::string& text) {
  if (text.empty() || !(std::isalpha((unsigned char)text[0]) || text[0] == '_')) return false;
  for (char c : text)
    if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
  return true;
}

// directives that may stand at the start of a line and are not labels
bool isDirective(const std::string& word) {
  static const std::set<std::string> names = {"processor", "include", "incbin", "incdir", "org", "rorg", "rend", "seg", "seg.u", "mac", "endm", "macro", "if", "ifconst", "ifnconst", "else", "endif", "eif",
                                              "echo", "repeat", "repend", "align", "subroutine", "ds", "dc", "dc.b", "dc.w", "byte", "word", "hex", "err", "list", "equ", "set", "eqm", "end",
                                              ".byte", ".word", ".ds", ".dc", ".org", ".align"};
  return names.count(lowered(word)) > 0;
}

bool definesData(const std::string& word) {
  static const std::set<std::string> names = {"equ", "=", "set", "eqm", "ds", ".ds", "dc", "dc.b", "dc.w", ".byte", ".word", "byte", "word", "hex", "mac", "macro"};
  return names.count(lowered(word)) > 0;
}

std::string capped(const std::string& text, size_t limit) { return text.size() <= limit ? text : utf8Cut(text, limit) + "..."; }

// The first JSON object in what the model wrote.
json objectIn(const std::string& text) {
  size_t open = text.find('{'), close = text.rfind('}');
  if (open == std::string::npos || close == std::string::npos || close < open) return json();
  json value = json::parse(text.substr(open, close - open + 1), nullptr, false);
  return value.is_object() ? value : json();
}

}  // namespace

bool Guided::applies(const GuidedInput& input, std::string* why) {
  std::string extension = lowered(std::filesystem::path(input.file).extension().string());
  if (input.batariBasic || (extension != ".asm" && extension != ".s" && extension != ".a65" && extension != ".inc")) {
    if (why) *why = "the guided procedure works on a DASM assembly file: open the one to change";
    return false;
  }
  if (!input.kb || input.fileText.find_first_not_of(" \t\r\n") == std::string::npos) {
    if (why) *why = "there is no assembly file in front to change";
    return false;
  }
  return true;
}

bool Guided::start(const GuidedInput& input, GuidedStep* step, std::string* why) {
  if (!applies(input, why)) return false;
  input_ = input;
  lines_.clear(), routines_.clear(), variables_.clear(), recipes_.clear();
  variablesLine_ = 0, stage_ = 0, attempts_ = 0;
  routine_.clear(), lastAnswer_.clear();
  {
    std::istringstream in(input.fileText);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines_.push_back(line);
    }
  }
  // the routines (a label followed by instructions) and the RAM bytes the file declares before its code starts
  bool codeSeen = false;
  int instructions = 0;
  for (size_t i = 0; i < lines_.size(); i++) {
    std::vector<std::string> words = wordsOf(codeOf(lines_[i]));
    if (words.empty()) continue;
    bool atStart = !lines_[i].empty() && lines_[i][0] != ' ' && lines_[i][0] != '\t';
    std::string first = words[0];
    if (!first.empty() && first.back() == ':') first.pop_back();
    if (atStart && isIdentifier(first) && !isDirective(first) && !input.kb->instruction(first)) {
      std::string second = words.size() > 1 ? words[1] : "";
      if (lowered(second) == "ds" || lowered(second) == ".ds") {
        variables_.push_back(first);
        if (!codeSeen) variablesLine_ = (int)i + 1;
        continue;
      }
      if (definesData(second)) continue;
      if (!routines_.empty()) routines_.back().last = (int)i;
      if (!routines_.empty() && instructions < 2) routines_.pop_back();  // a label over data, or an empty one
      instructions = 0;
      Routine routine;
      routine.name = first, routine.first = (int)i + 1, routine.last = (int)lines_.size();
      size_t semicolon = lines_[i].find(';');
      if (semicolon != std::string::npos) routine.comment = trimmed(lines_[i].substr(semicolon + 1));
      else if (i > 0 && trimmed(lines_[i - 1]).rfind(";", 0) == 0) routine.comment = trimmed(trimmed(lines_[i - 1]).substr(1));
      routines_.push_back(routine);
      words.erase(words.begin());
      if (words.empty()) continue;
      first = words[0];
    }
    if (input.kb->instruction(first)) instructions++, codeSeen = true;
  }
  if (!routines_.empty() && instructions < 2) routines_.pop_back();
  if (routines_.empty()) {
    if (why) *why = "the file has no routine (a label followed by instructions) to put the change in";
    return false;
  }
  stage_ = 1;
  *step = recipeStep();
  return true;
}

GuidedStep Guided::recipeStep() const {
  GuidedStep step;
  step.label = "Choosing the recipe";
  step.system = kSystem;
  ojson ids = ojson::array();
  std::string list;
  for (auto& technique : input_.kb->techniques()) {
    if (technique.id.rfind("7800-", 0) == 0 || technique.id.rfind("tinyelf-", 0) == 0) continue;  // other machines and languages
    ids.push_back(technique.id);
    list += "- " + technique.id + ": " + technique.title + "\n";
  }
  step.prompt = "The user asks: \"" + input_.request + "\"\n\nThese are the recipes of the reference:\n" + list +
                "\nWhich of them are needed to do what the user asks? Name the one or two that fit best, the most specific first; an empty list if none fits.\n"
                "Answer with JSON: {\"recipes\": [\"<id>\", ...]}";
  step.schema = ojson{{"type", "object"}, {"properties", ojson{{"recipes", ojson{{"type", "array"}, {"items", ojson{{"type", "string"}, {"enum", ids}}}, {"maxItems", 2}}}}}, {"required", ojson::array({"recipes"})}};
  step.maxTokens = 80;
  return step;
}

GuidedStep Guided::routineStep() const {
  GuidedStep step;
  step.label = "Choosing the routine";
  step.system = kSystem;
  ojson names = ojson::array();
  std::string list;
  for (auto& routine : routines_) {
    names.push_back(routine.name);
    list += "- " + routine.name + " (lines " + std::to_string(routine.first) + "-" + std::to_string(routine.last) + ")" + (routine.comment.empty() ? "" : ": " + capped(routine.comment, 90)) + "\n";
    if (list.size() > 6000) break;
  }
  std::string recipes;
  for (auto& technique : input_.kb->techniques())
    if (std::find(recipes_.begin(), recipes_.end(), technique.id) != recipes_.end()) recipes += "- " + technique.title + ": " + capped(technique.summary, 300) + "\n";
  step.prompt = "The user asks: \"" + input_.request + "\"\n\n" + (recipes.empty() ? "" : "What the reference says:\n" + recipes + "\n") +
                (input_.notes.empty() ? "" : "Notes from the author of the project (follow them):\n" + capped(input_.notes, 1800) + "\n\n") + "The routines of " + input_.file + ":\n" + list +
                "\nIn which routine do the new lines go? Code that reads the controls or changes the game's state runs once per frame, outside the picture: it goes in the routine that already does that "
                "kind of thing. Never choose a routine that draws the screen (those are counted cycle by cycle).\n"
                "Answer with JSON: {\"routine\": \"<name>\"}";
  step.schema = ojson{{"type", "object"}, {"properties", ojson{{"routine", ojson{{"type", "string"}, {"enum", names}}}}}, {"required", ojson::array({"routine"})}};
  step.maxTokens = 60;
  return step;
}

ojson Guided::actionSchema() {
  return ojson{{"type", "object"},
               {"properties",
                ojson{{"do", ojson{{"type", "string"}, {"enum", ojson::array({"inc", "dec", "set", "add", "sub", "write", "call", "goto"})}}},
                      {"target", ojson{{"type", "string"}}},
                      {"value", ojson{{"type", "string"}}},
                      {"a", ojson{{"type", "string"}}},
                      {"x", ojson{{"type", "string"}}},
                      {"y", ojson{{"type", "string"}}}}},
               {"required", ojson::array({"do", "target"})}};
}

// One action of a slot as assembly lines. A RAM variable the file does not have is added to *newVariables. Returns what is wrong
// with it, or "" if it could be written.
std::string Guided::actionLines(const json& action, const std::string& indent, std::vector<std::string>* lines, std::vector<std::string>* newVariables) const {
  auto text = [&](const char* key) {
    if (!action.contains(key)) return std::string();
    const json& value = action[key];
    return value.is_string() ? trimmed(value.get<std::string>()) : value.is_number_integer() ? std::to_string(value.get<long long>()) : std::string();
  };
  std::string what = lowered(text("do")), target = text("target"), value = text("value");
  while (!target.empty() && (target[0] == '#' || target[0] == '.')) target.erase(target.begin());
  while (!value.empty() && value[0] == '#') value.erase(value.begin());
  auto isNumber = [](const std::string& written) {
    if (written.empty()) return false;
    if (written[0] == '$') return written.size() > 1 && written.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos;
    if (written[0] == '%') return written.size() > 1 && written.find_first_not_of("01", 1) == std::string::npos;
    return written.find_first_not_of("0123456789") == std::string::npos;
  };
  auto isVariable = [&](const std::string& name) { return std::find(variables_.begin(), variables_.end(), name) != variables_.end() || std::find(newVariables->begin(), newVariables->end(), name) != newVariables->end(); };
  auto isLabel = [&](const std::string& name) {
    for (auto& routine : routines_)
      if (routine.name == name) return true;
    // any other label or constant of the file or the project
    return isIdentifier(name) && input_.inProject && input_.inProject(lowered(name)) && !input_.kb->reg(name) && !input_.kb->instruction(name);
  };
  // "#5", "5", "$1E", a constant of the project, or a variable: the operand of a load
  auto operand = [&](const std::string& written, std::string* out) {
    if (isNumber(written)) return *out = "#" + written, true;
    if (isVariable(written)) return *out = written, true;
    if (isLabel(written)) return *out = "#" + written, true;  // a constant
    return false;
  };
  auto variable = [&](std::string* wrong) {
    if (!isIdentifier(target) || input_.kb->reg(target) || input_.kb->instruction(target)) return *wrong = "\"" + target + "\" is not a name for a RAM variable (for a register of the 2600 use \"write\")", false;
    if (!isVariable(target)) {
      if (isLabel(target)) return *wrong = "\"" + target + "\" is a label or a constant of the file, not a RAM variable", false;
      newVariables->push_back(target);
    }
    return true;
  };
  std::string wrong, loaded;
  if (what == "inc" || what == "dec") {
    if (!variable(&wrong)) return wrong;
    lines->push_back(indent + what + " " + target);
  } else if (what == "set") {
    if (!variable(&wrong)) return wrong;
    if (!operand(value, &loaded)) return "\"value\" of set must be a number, a constant or a variable of the file; got \"" + value + "\"";
    lines->push_back(indent + "lda " + loaded);
    lines->push_back(indent + "sta " + target);
  } else if (what == "add" || what == "sub") {
    if (!variable(&wrong)) return wrong;
    if (value.size() > 1 && value[0] == '-') value.erase(value.begin()), what = what == "add" ? "sub" : "add";  // add -1 is sub 1
    if (!operand(value, &loaded)) return "\"value\" of " + what + " must be a number, a constant or a variable of the file; got \"" + value + "\"";
    lines->push_back(indent + "lda " + target);
    lines->push_back(indent + (what == "add" ? "clc" : "sec"));
    lines->push_back(indent + (what == "add" ? "adc " : "sbc ") + loaded);
    lines->push_back(indent + "sta " + target);
  } else if (what == "write") {
    const Register* reg = input_.kb->reg(target);
    if (!reg) return "\"" + target + "\" is not a register of the 2600 (for a RAM variable use \"set\")";
    if (!operand(value, &loaded)) return "\"value\" of write must be a number, a constant or a variable of the file; got \"" + value + "\"";
    lines->push_back(indent + "lda " + loaded);
    lines->push_back(indent + "sta " + reg->name);
  } else if (what == "call" || what == "goto") {
    bool known = false;
    for (auto& routine : routines_) known = known || routine.name == target;
    if (!known && !isLabel(target)) {
      std::string names;
      for (size_t i = 0; i < routines_.size() && i < 40; i++) names += (i ? ", " : "") + routines_[i].name;
      return "\"" + target + "\" is not a routine or a label of the file. The file has: " + names;
    }
    if (what == "call")
      for (const char* registerName : {"a", "x", "y"}) {
        std::string given = text(registerName);
        while (!given.empty() && given[0] == '#') given.erase(given.begin());
        if (given.empty()) continue;
        if (!operand(given, &loaded)) return std::string("\"") + registerName + "\" of call must be a number, a constant or a variable of the file; got \"" + given + "\"";
        lines->push_back(indent + "ld" + registerName + " " + loaded);
      }
    lines->push_back(indent + (what == "call" ? "jsr " : "jmp ") + target);
  } else {
    return "\"do\" must be one of inc, dec, set, add, sub, write, call, goto";
  }
  return "";
}

std::vector<std::pair<std::string, const RecipeTemplate*>> Guided::variants() const {
  std::vector<std::pair<std::string, const RecipeTemplate*>> list;
  for (auto& id : recipes_)
    for (auto& technique : input_.kb->techniques())
      if (technique.id == id)
        for (auto& recipe : technique.templates) list.push_back({technique.id + "/" + recipe.id, &recipe});
  return list;
}

GuidedStep Guided::codeStep(const std::string& problems) const {
  GuidedStep step;
  step.label = attempts_ == 0 ? "Writing the change" : "Correcting the change";
  step.system = kSystem;
  const Routine* routine = nullptr;
  for (auto& item : routines_)
    if (item.name == routine_) routine = &item;
  int first = routine ? routine->first : 1, last = routine ? std::min(routine->last, routine->first + 139) : 1;
  std::string listing;
  for (int line = first; line <= last && line <= (int)lines_.size(); line++) listing += std::to_string(line) + ": " + lines_[line - 1] + "\n";
  std::string recipes;
  for (auto& technique : input_.kb->techniques()) {
    if (std::find(recipes_.begin(), recipes_.end(), technique.id) == recipes_.end()) continue;
    recipes += "[" + technique.id + "] " + technique.title + "\n" + technique.summary + "\n";
    for (auto& stepText : technique.steps) recipes += "- " + stepText + "\n";
    if (!technique.example.empty()) recipes += "Example (its names are examples: use the file's names, or declare yours):\n" + capped(technique.example, 1400) + "\n";
    recipes += "\n";
  }
  std::string variables;
  for (size_t i = 0; i < variables_.size() && i < 70; i++) variables += (i ? ", " : "") + variables_[i];
  step.temperature = attempts_ == 0 ? 0.1 : attempts_ == 1 ? 0.5 : 0.8;
  auto available = variants();
  if (!available.empty()) {
    // recipes with holes: the branches and labels are written; the model says which variant and what goes in each hole
    ojson names = ojson::array(), slotProperties = ojson::object(), slotNames = ojson::array();
    std::string shown;
    for (auto& item : available) {
      names.push_back(item.first);
      shown += "Variant \"" + item.first + "\": " + item.second->title + "\n";
      for (auto& line : item.second->code) {
        std::string text = trimmed(line);
        if (text.size() > 2 && text.front() == '{' && text.back() == '}' && text.find(' ') == std::string::npos && text != "{L}") {
          std::string slot = text.substr(1, text.size() - 2), purpose;
          for (auto& known : item.second->slots)
            if (known.first == slot) purpose = known.second;
          shown += "        <slot " + slot + ": your lines; " + purpose + ">\n";
          if (!slotProperties.contains(slot)) slotNames.push_back(slot);
          slotProperties[slot] = ojson{{"type", "array"}, {"items", actionSchema()}, {"maxItems", 8}};
        } else {
          shown += (text.rfind(".", 0) == 0 ? "" : "        ") + text + "\n";
        }
      }
      shown += "\n";
    }
    names.push_back("write-my-own");
    step.prompt = "The user asks: \"" + input_.request + "\"\n\n" + (input_.notes.empty() ? "" : "Notes from the author of the project (follow them):\n" + capped(input_.notes, 1800) + "\n\n") +
                  "The routine " + routine_ + " of " + input_.file + ", with its line numbers:\n" + listing + "\n" + (variables.empty() ? "" : "RAM variables the file already has: " + variables + "\n\n") +
                  "The reference has this code ready, with slots for you to fill ({L} becomes a label prefix; VCSIDE declares the variables in braces):\n\n" + shown +
                  "Choose the variant that does what the user asks and say what happens in its slots. A slot is a list of ACTIONS, each one a JSON object; you do not write assembly:\n"
                  "  {\"do\": \"inc\", \"target\": \"Presses\"}                    adds 1 to a RAM variable: this is how something is COUNTED (\"dec\" takes 1). A name the file does not have is declared for you.\n"
                  "  {\"do\": \"set\", \"target\": \"Jumping\", \"value\": \"1\"}       puts a number (or a constant, or another variable) in a RAM variable.\n"
                  "  {\"do\": \"add\", \"target\": \"PlayerX\", \"value\": \"2\"}       adds a number to a RAM variable (\"sub\" takes it away).\n"
                  "  {\"do\": \"write\", \"target\": \"AUDV0\", \"value\": \"8\"}       writes a number to a register of the 2600 (a tone: AUDC0 4, AUDF0 10, AUDV0 8; silence: AUDV0 0).\n"
                  "  {\"do\": \"call\", \"target\": \"SomeRoutine\", \"y\": \"SomeConstant\"}  calls a routine of the file (jsr), with A, X or Y loaded first if you give \"a\", \"x\" or \"y\".\n"
                  "  {\"do\": \"goto\", \"target\": \"Reset\"}                     jumps to a label of the file and does not come back (for example to restart).\n"
                  "Rules:\n"
                  "- \"variant\": the variant's name. (\"write-my-own\" only if none fits: then put assembly lines in \"new_lines\".)\n"
                  "- \"slots\": use exactly the slot names of the variant you chose; for each, its actions in order; a slot you do not need is an empty list. The variant already tests the button, the stick or the switch: "
                  "the actions are only what happens then.\n"
                  "- \"after_line\": the number of the line of the routine after which the code is inserted (a place every frame passes through, not inside a branch that is skipped).\n"
                  "- \"new_variables\": leave it empty (variables named in actions are declared for you).\n"
                  "- Registers, routines and labels must exist: the 2600's registers, the names of the file.\n"
                  "- \"summary\": one sentence for the user: what it does and what is left to do, if anything.\n"
                  "Answer with JSON: {\"variant\": \"...\", \"after_line\": <number>, \"slots\": {\"<SLOT>\": [{\"do\": \"...\", \"target\": \"...\"}]}, \"new_lines\": [], \"new_variables\": [], \"summary\": \"...\"}";
    if (!problems.empty())
      step.prompt += "\n\nYour previous answer was:\n" + capped(lastAnswer_, 1500) + "\nIt cannot be used:\n" + problems + "Answer again with the whole corrected JSON.";
    // the keys in the order the model should decide them: the variant first, then where, then what
    step.schema = ojson{{"type", "object"},
                        {"properties",
                         ojson{{"variant", ojson{{"type", "string"}, {"enum", names}}},
                               {"after_line", ojson{{"type", "integer"}, {"minimum", first}, {"maximum", last}}},
                               // every slot is asked for (an empty list will do), in the template's order
                               {"slots", ojson{{"type", "object"}, {"properties", slotProperties}, {"required", slotNames}}},
                               {"new_lines", ojson{{"type", "array"}, {"items", ojson{{"type", "string"}}}, {"maxItems", 40}}},
                               {"new_variables", ojson{{"type", "array"}, {"items", ojson{{"type", "string"}}}, {"maxItems", 4}}},
                               {"summary", ojson{{"type", "string"}}}}},
                        {"required", ojson::array({"variant", "after_line", "slots", "new_variables", "summary"})}};
    step.maxTokens = 900;
    return step;
  }
  step.prompt = "The user asks: \"" + input_.request + "\"\n\n" + (recipes.empty() ? "" : "Recipes of the reference:\n" + recipes) +
                (input_.notes.empty() ? "" : "Notes from the author of the project (follow them):\n" + capped(input_.notes, 1800) + "\n\n") + "The routine " + routine_ + " of " + input_.file +
                ", with its line numbers:\n" + listing + "\n" + (variables.empty() ? "" : "RAM variables the file already has: " + variables + "\n\n") +
                "Write the lines to add to this routine. Rules:\n"
                "- \"after_line\": the number of the line after which your lines are inserted (they run right after it; choose a place every frame passes through, not inside a branch that is skipped).\n"
                "- \"new_lines\": your assembly lines in order, one per item, without line numbers. Local labels start with a dot (.Skip): every label you branch to must also be one of your lines. Do not repeat lines that are already there.\n"
                "- \"new_variables\": the names of the RAM bytes your lines need and the file does not have (one byte each); VCSIDE declares them. An empty list if none.\n"
                "- Only what the user asks, as small as it can be. If a part cannot be done in this one place, leave a comment line saying what is left.\n"
                "- \"summary\": one sentence for the user: what the lines do and what is left to do, if anything.\n"
                "Answer with JSON: {\"after_line\": <number>, \"new_lines\": [\"...\"], \"new_variables\": [\"...\"], \"summary\": \"...\"}";
  if (!problems.empty())
    step.prompt += "\n\nYour previous answer was:\n" + capped(lastAnswer_, 1500) + "\nIt cannot be used:\n" + problems + "Answer again with the whole corrected JSON.";
  step.schema = ojson{{"type", "object"},
                      {"properties",
                       ojson{{"after_line", ojson{{"type", "integer"}, {"minimum", first}, {"maximum", last}}},
                             {"new_lines", ojson{{"type", "array"}, {"items", ojson{{"type", "string"}}}, {"minItems", 1}, {"maxItems", 40}}},
                             {"new_variables", ojson{{"type", "array"}, {"items", ojson{{"type", "string"}}}, {"maxItems", 4}}},
                             {"summary", ojson{{"type", "string"}}}}},
                      {"required", ojson::array({"after_line", "new_lines", "new_variables", "summary"})}};
  step.maxTokens = 900;
  return step;
}

bool Guided::build(const json& answer, std::string* after, std::string* problems, int* firstLine, int* lastLine) const {
  const Routine* routine = nullptr;
  for (auto& item : routines_)
    if (item.name == routine_) routine = &item;
  if (!routine) return *problems = "- The routine is not in the file.\n", false;
  int afterLine = answer.value("after_line", 0);
  if (afterLine < routine->first || afterLine > routine->last || afterLine > (int)lines_.size()) {
    *problems = "- \"after_line\" must be a line of the routine: " + std::to_string(routine->first) + " to " + std::to_string(routine->last) + ".\n";
    return false;
  }
  // the indentation the routine's instructions have
  std::map<std::string, int> indents;
  for (int line = routine->first; line <= routine->last && line <= (int)lines_.size(); line++) {
    const std::string& text = lines_[line - 1];
    size_t at = text.find_first_not_of(" \t");
    if (at == std::string::npos || at == 0) continue;
    std::vector<std::string> words = wordsOf(codeOf(text));
    if (!words.empty() && input_.kb->instruction(words[0])) indents[text.substr(0, at)]++;
  }
  std::string indent = "\t";
  int most = 0;
  for (auto& item : indents)
    if (item.second > most) most = item.second, indent = item.first;
  // the new lines, placed as DASM wants them: labels at the start of the line, instructions indented
  std::vector<std::string> added;
  std::vector<std::string> templateVariables;
  const RecipeTemplate* chosen = nullptr;
  std::string variant = answer.value("variant", std::string());
  for (auto& item : variants())
    if (item.first == variant) chosen = item.second;
  if (chosen) {
    // a label prefix that is free in the file
    std::string prefix = variant.substr(0, variant.find('-'));
    prefix = prefix.substr(0, prefix.find('/'));
    if (!prefix.empty()) prefix[0] = (char)std::toupper((unsigned char)prefix[0]);
    std::string base = prefix;
    for (int number = 2; input_.fileText.find("." + prefix) != std::string::npos && number < 50; number++) prefix = base + std::to_string(number);
    templateVariables = chosen->variables;  // (the actions add theirs)
    // what the model put in the slots; a slot's name from another variant is taken for the empty slot it must have meant
    json filled = answer.contains("slots") && answer["slots"].is_object() ? answer["slots"] : json::object();
    {
      std::vector<std::string> strays;
      for (auto it = filled.begin(); it != filled.end(); ++it) {
        bool own = false;
        for (auto& slot : chosen->slots) own = own || slot.first == it.key();
        if (!own && it.value().is_array() && !it.value().empty()) strays.push_back(it.key());
      }
      size_t next = 0;
      for (auto& slot : chosen->slots) {
        bool empty = !filled.contains(slot.first) || !filled[slot.first].is_array() || filled[slot.first].empty();
        if (empty && next < strays.size()) filled[slot.first] = filled[strays[next++]];
      }
    }
    int usedActions = 0;
    dropped_.clear();
    static const std::set<std::string> flow = {"bcc", "bcs", "beq", "bne", "bmi", "bpl", "bvc", "bvs", "rts", "rti", "brk"};
    for (auto& line : chosen->code) {
      std::string text = trimmed(line);
      bool slot = text.size() > 2 && text.front() == '{' && text.back() == '}' && text.find(' ') == std::string::npos && text != "{L}";
      if (slot) {
        std::string name = text.substr(1, text.size() - 2);
        if (!filled.contains(name) || !filled[name].is_array()) continue;
        for (auto& item : filled[name]) {
          if (item.is_object()) {  // an action: VCSIDE writes its assembly
            std::string wrong = actionLines(item, indent, &added, &templateVariables);
            if (!wrong.empty() && attempts_ >= 3) {  // the last try: the action is left out and said, the rest is kept
              dropped_ += (dropped_.empty() ? "" : "; ") + item.dump() + " (" + wrong + ")";
              continue;
            }
            if (!wrong.empty()) return *problems = "- In the slot " + name + ": " + wrong + "\n", false;
            usedActions++;
            continue;
          }
          usedActions++;
          if (!item.is_string()) continue;
          std::istringstream in(item.get<std::string>());
          for (std::string raw; std::getline(in, raw);) {
            std::string content = trimmed(raw);
            if (content.empty() || content.rfind("```", 0) == 0) continue;
            if (content[0] == ';') {
              added.push_back(indent + content);
              continue;
            }
            std::vector<std::string> words = wordsOf(codeOf(content));
            std::string first = words.empty() ? "" : lowered(words[0]);
            if (!input_.kb->instruction(first) || flow.count(first))
              return *problems = "- In the slot " + name + ", the line \"" + content + "\" is a label, a branch or a return. A slot takes straight code only (lda, sta, inc, jsr...): the variant already has the branches and the labels.\n", false;
            added.push_back(indent + content);
          }
        }
        continue;
      }
      // {L} and {Variable}
      for (size_t at = text.find("{L}"); at != std::string::npos; at = text.find("{L}")) text.replace(at, 3, prefix);
      for (auto& name : chosen->variables)
        for (size_t at = text.find("{" + name + "}"); at != std::string::npos; at = text.find("{" + name + "}")) text.replace(at, name.size() + 2, name);
      added.push_back(text.rfind(".", 0) == 0 ? text : indent + text);
    }
    // the model's lines ran into nothing? a variant with every slot empty does nothing
    bool any = chosen->optional || usedActions > 0;
    left_ = chosen->left;
    if (!any) return *problems = "- Every slot of the variant is empty: write in \"slots\" the lines that do what the user asks.\n", false;
  } else if (!variant.empty() && variant != "write-my-own") {
    return *problems = "- \"variant\" must be one of the variants shown, or \"write-my-own\".\n", false;
  }
  if (!chosen && (!answer.contains("new_lines") || !answer["new_lines"].is_array())) return *problems = "- \"new_lines\" must be a list of assembly lines.\n", false;
  if (!chosen)
  for (auto& item : answer["new_lines"]) {
    if (!item.is_string()) continue;
    std::istringstream in(item.get<std::string>());  // an item that holds several lines
    for (std::string raw; std::getline(in, raw);) {
      std::string content = trimmed(raw);
      if (content.rfind("```", 0) == 0) continue;
      // a line number copied from the listing
      size_t digits = 0;
      while (digits < content.size() && std::isdigit((unsigned char)content[digits])) digits++;
      if (digits > 0 && digits < content.size() && content[digits] == ':') content = trimmed(content.substr(digits + 1));
      if (content.empty()) continue;
      if (content[0] == ';') {
        added.push_back(indent + content);
        continue;
      }
      std::vector<std::string> words = wordsOf(codeOf(content));
      std::string first = words.empty() ? "" : words[0];
      if (input_.kb->instruction(first) || isDirective(first)) {
        added.push_back(indent + content);
        continue;
      }
      // a label, alone or in front of an instruction
      std::string label = first;
      if (!label.empty() && label.back() == ':') label.pop_back();
      std::string rest = trimmed(content.substr(content.find(first) + first.size()));
      added.push_back(label);
      if (!rest.empty()) added.push_back(indent + rest);
    }
  }
  if (added.empty()) return *problems = "- \"new_lines\" is empty.\n", false;
  // the variables to declare
  std::vector<std::string> declared;
  for (auto& name : templateVariables)
    if (std::find(variables_.begin(), variables_.end(), name) == variables_.end()) declared.push_back(name);
  if (answer.contains("new_variables") && answer["new_variables"].is_array())
    for (auto& item : answer["new_variables"]) {
      if (!item.is_string()) continue;
      std::string name = trimmed(item.get<std::string>());
      if (name.empty()) continue;
      if (!isIdentifier(name)) return *problems = "- \"" + name + "\" is not a name for a variable (letters, digits and _).\n", false;
      if (input_.kb->reg(name) || input_.kb->instruction(name)) return *problems = "- \"" + name + "\" is a register or an instruction of the 2600, not a name for your variable.\n", false;
      bool exists = std::find(variables_.begin(), variables_.end(), name) != variables_.end();
      for (auto& item2 : routines_) exists = exists || item2.name == name;
      if (!exists && std::find(declared.begin(), declared.end(), name) == declared.end()) declared.push_back(name);
    }
  if (!declared.empty() && variablesLine_ == 0)
    return *problems = "- The file declares no RAM variables (NAME DS 1) that VCSIDE can add yours to: use the variables it has, or none.\n", false;
  std::vector<std::string> declarations;
  if (!declared.empty()) {
    // written like the last one the file has: its name's column and its DS
    const std::string& model = lines_[variablesLine_ - 1];
    size_t nameEnd = model.find_first_of(" \t");
    size_t keyword = nameEnd == std::string::npos ? std::string::npos : model.find_first_not_of(" \t", nameEnd);
    std::string gap = nameEnd == std::string::npos || keyword == std::string::npos ? "\t" : model.substr(nameEnd, keyword - nameEnd);
    std::string word = keyword == std::string::npos ? "DS" : model.substr(keyword, model.find_first_of(" \t", keyword) - keyword);
    for (auto& name : declared) declarations.push_back(name + (gap.find('\t') != std::string::npos ? gap : std::string(std::max<size_t>(1, nameEnd + gap.size() > name.size() ? nameEnd + gap.size() - name.size() : 1), ' ')) + word + " 1");
  }
  std::vector<std::string> result;
  for (int line = 1; line <= (int)lines_.size(); line++) {
    result.push_back(lines_[line - 1]);
    if (line == variablesLine_)
      for (auto& declaration : declarations) result.push_back(declaration);
    if (line == afterLine)
      for (auto& text : added) result.push_back(text);
  }
  bool crlf = input_.fileText.find("\r\n") != std::string::npos;
  std::string joined;
  for (size_t i = 0; i < result.size(); i++) joined += result[i] + (i + 1 < result.size() || (!input_.fileText.empty() && input_.fileText.back() == '\n') ? (crlf ? "\r\n" : "\n") : "");
  *after = joined;
  int shift = variablesLine_ > 0 && variablesLine_ < afterLine ? (int)declarations.size() : 0;
  *firstLine = afterLine + 1 + shift;
  *lastLine = afterLine + (int)added.size() + shift;
  // the checks of any proposed change, and the assembler
  *problems = proposalProblems(input_.request, input_.fileText, joined, *input_.kb, input_.inProject ? input_.inProject : [](const std::string&) { return false; }, false);
  if (problems->empty() && input_.assemble) {
    std::string errors, before;
    if (input_.assemble(input_.file, joined, &errors) && !errors.empty() && !(input_.assemble(input_.file, input_.fileText, &before) && !before.empty()))
      *problems = "- The project does not assemble with your lines. The assembler says:\n" + errors;
  }
  return problems->empty();
}

Guided::Outcome Guided::answer(const std::string& text, GuidedStep* next, EditProposal* proposal, std::string* message) {
  json value = objectIn(text);
  if (stage_ == 1) {
    recipes_.clear();
    if (value.contains("recipes") && value["recipes"].is_array())
      for (auto& item : value["recipes"]) {
        if (!item.is_string()) continue;
        for (auto& technique : input_.kb->techniques())
          if (technique.id == item.get<std::string>() && std::find(recipes_.begin(), recipes_.end(), technique.id) == recipes_.end()) recipes_.push_back(technique.id);
      }
    stage_ = 2;
    *next = routineStep();
    return Outcome::Ask;
  }
  if (stage_ == 2) {
    std::string name = value.value("routine", std::string());
    bool known = false;
    for (auto& routine : routines_) known = known || routine.name == name;
    if (!known) {
      *message = "the model did not choose a routine of the file";
      return Outcome::Failed;
    }
    routine_ = name;
    stage_ = 3, attempts_ = 0;
    *next = codeStep("");
    return Outcome::Ask;
  }
  if (stage_ != 3) {
    *message = "the procedure was not started";
    return Outcome::Failed;
  }
  attempts_++;
  lastAnswer_ = value.is_object() ? value.dump() : text;
  std::string after, problems;
  int firstLine = 0, lastLine = 0;
  bool good = value.is_object() ? build(value, &after, &problems, &firstLine, &lastLine) : (problems = "- The answer was not the JSON object asked for.\n", false);
  if (!good) {
    if (attempts_ >= 3) {
      *message = "after three tries the change still has problems:\n" + problems;
      return Outcome::Failed;
    }
    *next = codeStep(problems);
    return Outcome::Ask;
  }
  proposal->path = (std::filesystem::path(input_.projectDir) / input_.file).string();
  proposal->relative = input_.file;
  proposal->before = input_.fileText, proposal->after = after;
  proposal->created = false, proposal->changes = 1;
  proposal->firstLine = firstLine, proposal->lastLine = lastLine;
  *message = value.value("summary", std::string());
  if (!left_.empty()) *message += (message->empty() ? "" : " ") + left_;
  if (!dropped_.empty()) *message += (message->empty() ? "" : " ") + std::string("Left out, because it could not be written: ") + dropped_ + ".";
  return Outcome::Proposal;
}

}  // namespace vcs::assistant
