// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/domain_checks.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <vector>

#include "assistant/validate.h"

namespace vcs::assistant {

namespace {
std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return text;
}

bool hasWord(const std::string& text, const std::string& word) {  // a whole word, case sensitive
  for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
    bool before = at == 0 || !(std::isalnum((unsigned char)text[at - 1]) || text[at - 1] == '_');
    bool after = at + word.size() >= text.size() || !(std::isalnum((unsigned char)text[at + word.size()]) || text[at + word.size()] == '_');
    if (before && after) return true;
  }
  return false;
}

// the code of a line, without its comment
std::string withoutComment(const std::string& line) {
  size_t semicolon = line.find(';');
  return semicolon == std::string::npos ? line : line.substr(0, semicolon);
}
}  // namespace

bool asksForNewFile(const std::string& request) {
  std::string text = lowered(request);
  for (const char* word : {"new file", "nuevo archivo", "nuevo fichero", "nueva carpeta", "create a file", "crea un archivo", "crea un fichero", "crea el archivo", "crea el fichero", "create the file", "split", "separate file", "archivo aparte", "fichero aparte"})
    if (text.find(word) != std::string::npos) return true;
  return false;
}

std::string addedLines(const std::string& before, const std::string& after) {
  std::multiset<std::string> had;
  std::istringstream old(before);
  std::string line;
  while (std::getline(old, line)) had.insert(line);
  std::ostringstream added;
  std::istringstream now(after);
  while (std::getline(now, line)) {
    auto found = had.find(line);
    if (found != had.end()) had.erase(found);
    else added << line << "\n";
  }
  return added.str();
}

namespace {
// the lines `before` has and `after` has not
std::vector<std::string> removedLineList(const std::string& before, const std::string& after) {
  std::multiset<std::string> kept;
  std::istringstream now(after);
  std::string line;
  while (std::getline(now, line)) kept.insert(line);
  std::vector<std::string> gone;
  std::istringstream old(before);
  while (std::getline(old, line)) {
    auto found = kept.find(line);
    if (found != kept.end()) kept.erase(found);
    else gone.push_back(line);
  }
  return gone;
}

bool isBlankOrComment(const std::string& line) {
  size_t at = line.find_first_not_of(" \t\r");
  return at == std::string::npos || line[at] == ';';
}

// labels, constants and variables the text defines: a name at the start of a line, or followed by EQU, =, DS or SET
std::set<std::string> definedNames(const std::string& text) {
  std::set<std::string> names;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::string code = withoutComment(line);
    if (code.empty()) continue;
    size_t i = 0;
    if (!(std::isalpha((unsigned char)code[0]) || code[0] == '_' || code[0] == '.')) {
      // an indented name followed by EQU/=/DS/SET is a definition too
      i = code.find_first_not_of(" \t");
      if (i == std::string::npos) continue;
    }
    size_t end = i;
    while (end < code.size() && (std::isalnum((unsigned char)code[end]) || code[end] == '_' || code[end] == '.')) end++;
    if (end == i) continue;
    std::string name = code.substr(i, end - i);
    bool atStart = i == 0;
    std::istringstream rest(code.substr(end));
    std::string next;
    rest >> next;
    std::string upper = next;
    for (auto& c : upper) c = (char)std::toupper((unsigned char)c);
    if (atStart || upper == "EQU" || upper == "=" || upper == "DS" || upper == "SET" || upper == ".DS") names.insert(name);
  }
  return names;
}
}  // namespace

std::string proposalProblems(const std::string& request, const std::string& before, const std::string& after, const KnowledgeBase& kb,
                             const std::function<bool(const std::string&)>& inProject, bool batariBasic) {
  auto withoutSpaces = [](const std::string& text) {
    std::string kept;
    for (char c : text)
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') kept += c;
    return kept;
  };
  if (withoutSpaces(before) == withoutSpaces(after))
    return "- The change changes nothing: the new text is the old one with other spaces. Put the code you add in \"replace\" (keep the line you search for and add the new lines after it).\n";
  std::string added = addedLines(before, after), problems;
  if (added.empty()) return "";
  std::string asked = lowered(request);
  // the fire button is not in the joystick port: it is INPT4 (player 0) and INPT5 (player 1), bit 7, 0 when pressed
  bool aboutFire = false;
  for (const char* word : {"botón", "boton", "button", "disparo", "fire", "trigger", "pulse", "pulsa", "press"})
    aboutFire = aboutFire || asked.find(word) != std::string::npos;
  std::string code;
  {
    std::istringstream in(added);
    std::string line;
    while (std::getline(in, line)) code += withoutComment(line) + "\n";
  }
  bool readsPort = hasWord(code, "SWCHA"), readsFire = hasWord(code, "INPT4") || hasWord(code, "INPT5");
  if (!readsPort && !readsFire) {
    // the change tests bits right after a read of SWCHA that was already there ("lda SWCHA" / "and #%00000001  ; fire button"): look at the lines before the first added one
    std::set<std::string> had;
    {
      std::istringstream old(before);
      std::string line;
      while (std::getline(old, line)) had.insert(line);
    }
    std::vector<std::string> lines;
    {
      std::istringstream now(after);
      std::string line;
      while (std::getline(now, line)) lines.push_back(line);
    }
    size_t firstAdded = lines.size();
    for (size_t i = 0; i < lines.size() && firstAdded == lines.size(); i++)
      if (!had.count(lines[i])) firstAdded = i;
    std::string talksAboutFire = lowered(added);
    bool mentionsFire = talksAboutFire.find("fire") != std::string::npos || talksAboutFire.find("button") != std::string::npos || talksAboutFire.find("disparo") != std::string::npos ||
                        talksAboutFire.find("botón") != std::string::npos || talksAboutFire.find("boton") != std::string::npos;
    for (size_t i = firstAdded > 10 ? firstAdded - 10 : 0; i < firstAdded; i++) readsPort = readsPort || (mentionsFire && hasWord(withoutComment(lines[i]), "SWCHA"));
  }
  {
    // a change that deals with the button (it says so in a comment or a label) and reads neither INPT4 nor INPT5 has found the button somewhere it is not
    std::string addedLower = lowered(added);
    bool addedMentionsFire = addedLower.find("fire") != std::string::npos || addedLower.find("button") != std::string::npos || addedLower.find("disparo") != std::string::npos ||
                             addedLower.find("botón") != std::string::npos || addedLower.find("boton") != std::string::npos;
    readsPort = readsPort || addedMentionsFire;
  }
  if (aboutFire && readsPort && !readsFire && !batariBasic)
    problems += "- The fire button is not in SWCHA or any other register you named (SWCHA has only the four directions of each joystick). Read it from INPT4 (player 0) or INPT5 (player 1): "
                "bit 7 is 0 while the button is pressed (lda INPT4 / bmi NotPressed). To act once per press, keep the previous state in a RAM byte.\n";
  if (!batariBasic) {  // instructions of the 65C02 that the 2600's 6507 does not have
    static const std::set<std::string> only65c02 = {"bra", "phx", "phy", "plx", "ply", "stz", "trb", "tsb", "wai", "stp"};
    std::istringstream in(added);
    std::string line;
    std::set<std::string> seen;
    std::string list;
    while (std::getline(in, line)) {
      std::istringstream words(withoutComment(line));
      std::string first;
      words >> first;
      if (!first.empty() && (line[0] == ' ' || line[0] == '\t') == false) words >> first;  // a label first
      std::string op = lowered(first);
      if (only65c02.count(op) && seen.insert(op).second) list += (list.empty() ? "" : ", ") + op;
    }
    if (!list.empty())
      problems += "- The 2600's 6507 is a 6502: it has no " + list + " instruction (those are 65C02). Use jmp for bra, and the 6502 way for the others.\n";
  }
  if (readsFire && !batariBasic) {
    // INPT4/INPT5 bit 7 is 0 while pressed: bmi branches when the button is RELEASED, bpl when it is PRESSED. A label that says the opposite is a sign of the mix-up.
    std::istringstream in(added);
    std::string line, previous;
    while (std::getline(in, line)) {
      std::string code = withoutComment(line);
      std::istringstream words(code);
      std::string first, label;
      words >> first >> label;
      std::string op = lowered(first);
      if ((op == "bmi" || op == "bpl") && (hasWord(previous, "INPT4") || hasWord(previous, "INPT5"))) {
        std::string name = lowered(label);
        auto has = [&](std::initializer_list<const char*> parts) {
          for (const char* part : parts)
            if (name.find(part) != std::string::npos) return true;
          return false;
        };
        bool saysPressed = has({"press", "fire", "jump", "shoot", "pushed", "down"}) && !has({"not", "no", "release", "skip", "idle", "up"});
        bool saysReleased = has({"release", "notpress", "nofire", "nojump", "skip", "idle"});
        if (op == "bmi" && saysPressed)
          problems += "- After lda INPT4, bmi branches when the button is NOT pressed (bit 7 = 1 means released); \"" + label + "\" sounds like the pressed case. Use bpl to branch when it is pressed, or bmi to skip the code.\n";
        else if (op == "bpl" && saysReleased)
          problems += "- After lda INPT4, bpl branches when the button IS pressed (bit 7 = 0); \"" + label + "\" sounds like the released case. Use bmi to branch when it is released.\n";
      }
      if (!code.empty() && code.find_first_not_of(" \t") != std::string::npos) previous = code;
    }
  }
  {  // code that was there is not thrown away by a change that only adds something
    bool wantsRemoval = false;
    for (const char* word : {"quita", "elimina", "borra", "remove", "delete", "sustituye", "reemplaza", "replace", "cambia", "change", "arregla", "fix", "corrige", "refactor", "simplifica", "limpia", "clean", "mueve", "move", "renombra", "rename"})
      wantsRemoval = wantsRemoval || asked.find(word) != std::string::npos;
    std::vector<std::string> gone = removedLineList(before, after);
    int code = 0;
    for (auto& line : gone) code += !isBlankOrComment(line);
    if (!wantsRemoval && code >= 3)
      problems += "- Your change deletes " + std::to_string(code) + " lines of the existing code (for example \"" + [&] {
        for (auto& line : gone)
          if (!isBlankOrComment(line)) {
            size_t at = line.find_first_not_of(" \t");
            return line.substr(at);
          }
        return std::string();
      }() + "\") and the user did not ask for that. Keep them: the \"replace\" text must contain every old line you do not mean to change, with your new lines among them.\n";
  }
  if (!batariBasic) {  // a label or variable the code uses and nothing defines (mixed case ones: the uppercase ones are checked below)
    std::set<std::string> defined = definedNames(after);
    std::set<std::string> reported;
    std::istringstream in(added);
    std::string line;
    std::vector<std::string> missing;
    while (std::getline(in, line)) {
      std::string codeLine = withoutComment(line);
      size_t at = codeLine.find_first_not_of(" \t");
      if (at == std::string::npos) continue;
      std::istringstream words(codeLine.substr(at));
      std::string first, operand;
      words >> first;
      if (first.empty() || first.back() == ':' || !(std::islower((unsigned char)first[0]) || first.size() == 3)) {  // a label before the instruction: take the next word
        words >> first;
      }
      std::string op = lowered(first);
      static const std::set<std::string> uses = {"jsr", "jmp", "bne", "beq", "bcc", "bcs", "bmi", "bpl", "bvc", "bvs", "lda", "ldx", "ldy", "sta", "stx", "sty", "cmp", "cpx", "cpy",
                                                 "inc", "dec", "adc", "sbc", "and", "ora", "eor", "bit", "asl", "lsr", "rol", "ror"};
      if (!uses.count(op)) continue;
      words >> operand;
      while (!operand.empty() && (operand[0] == '#' || operand[0] == '<' || operand[0] == '>' || operand[0] == '(' || operand[0] == '[')) operand.erase(operand.begin());
      size_t end = 0;
      while (end < operand.size() && (std::isalnum((unsigned char)operand[end]) || operand[end] == '_' || operand[end] == '.')) end++;
      std::string name = operand.substr(0, end);
      if (name.empty() || !(std::isalpha((unsigned char)name[0]) || name[0] == '_' || name[0] == '.')) continue;
      bool hasLower = false;
      for (char c : name) hasLower = hasLower || std::islower((unsigned char)c);
      if (!hasLower || defined.count(name) || inProject(lowered(name)) || reported.count(name)) continue;
      reported.insert(name);
      missing.push_back(name);
    }
    if (!missing.empty()) {
      problems += "- These labels or variables are used and defined nowhere (not in the project, not in your change): ";
      for (size_t i = 0; i < missing.size() && i < 6; i++) problems += (i ? ", " : "") + missing[i];
      problems += ". Add the routine or the variable in your change (a label with its code; a variable as a line NAME DS 1 among the variables at the top) or use one that exists.\n";
    }
  }
  {
    auto unknown = unknownNames("```\n" + added + "```", kb, inProject, 6, batariBasic);
    if (!unknown.empty()) {
      problems += "- These names exist nowhere (not in the 2600's registers, not in the project, not defined by your change): ";
      for (size_t i = 0; i < unknown.size(); i++) problems += (i ? ", " : "") + unknown[i];
      problems += ". Define each one in your change (a label, NAME EQU value, or a RAM byte: a line NAME DS 1 among the variables at the top of the file) or use a name that exists.\n";
    }
  }
  return problems;
}

}  // namespace vcs::assistant
