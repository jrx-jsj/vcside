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
  if (aboutFire && readsPort && !readsFire && !batariBasic)
    problems += "- The fire button is not in SWCHA (that port has only the four directions of each joystick). Read it from INPT4 (player 0) or INPT5 (player 1): "
                "bit 7 is 0 while the button is pressed (lda INPT4 / bmi NotPressed). To act once per press, keep the previous state in a RAM byte.\n";
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
