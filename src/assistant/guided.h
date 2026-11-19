// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The guided procedure (docs/design-assistant.md, "The guided procedure"): how a small model on this computer makes a change
// to an assembly file. As an agent it has to find the place, quote it exactly, write the code, declare the variable and keep
// the rest, all at once, and it does not manage. Here VCSIDE leads and the model only chooses and fills in:
//   1. which recipes of the knowledge base apply (one or two of a list, or none);
//   2. which routine of the file the new code goes in (one of the file's labels);
//   3. after which line of that routine, the new lines, and the RAM bytes they need (the answer held to a JSON schema, so it
//      cannot come out malformed).
// VCSIDE then places the lines, declares the variables where the file declares its own, runs the checks of a proposed change
// and assembles the project in a copy; what fails goes back to step 3, twice at most. The result is a proposal for the user.
// No interface and no network here: a step is a question with the shape of its answer; who drives it asks the model.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "assistant/grounding.h"
#include "assistant/tools.h"
#include "kb.h"

namespace vcs::assistant {

struct GuidedInput {
  std::string request;      // what the user asked, in English if it was translated
  std::string projectDir;
  std::string file;         // the assembly file the user works on, relative to the project
  std::string fileText;     // as the editor holds it
  std::string notes;        // the project's VCSIDE.md, if any
  const KnowledgeBase* kb = nullptr;
  bool batariBasic = false;
  std::function<bool(const std::string& lowerCaseWord)> inProject;  // a name the project knows
  // the project built with the file's proposed text: false if it cannot be judged; *errors empty if it assembled
  std::function<bool(const std::string& relative, const std::string& text, std::string* errors)> assemble;
};

struct GuidedStep {
  std::string label;     // for the person watching: "Choosing the recipe"
  std::string system;    // the system prompt of this question
  std::string prompt;    // the question
  nlohmann::ordered_json schema;  // the shape of the answer (ordered: the grammar asks for the keys in this order)
  int maxTokens = 600;
  double temperature = 0.1;  // a second and a third try are asked with more, or the same answer comes back
};

class Guided {
 public:
  // Can this request be led this way? (an assembly file in front, with routines). *why says why not.
  static bool applies(const GuidedInput& input, std::string* why);

  // The first question. False (with *why) if the procedure does not apply.
  bool start(const GuidedInput& input, GuidedStep* step, std::string* why);

  enum class Outcome { Ask, Proposal, Failed };
  // The model's answer to the last question: the next question, the finished proposal (with *message: what it does, for the
  // user), or the reason it could not be done.
  Outcome answer(const std::string& text, GuidedStep* next, EditProposal* proposal, std::string* message);

  int stage() const { return stage_; }      // 1 recipes, 2 routine, 3 code
  int attempts() const { return attempts_; }  // how many times the code was asked for
  const std::vector<std::string>& recipes() const { return recipes_; }
  const std::string& routine() const { return routine_; }

 private:
  struct Routine {
    std::string name, comment;
    int first = 0, last = 0;  // lines, 1-based, inclusive
  };
  GuidedStep recipeStep() const;
  GuidedStep routineStep() const;
  GuidedStep codeStep(const std::string& problems) const;
  // the variants (recipes with holes) of the chosen recipes: "recipe/variant" and the template
  std::vector<std::pair<std::string, const RecipeTemplate*>> variants() const;
  // what goes in a slot: a list of actions (increment, set, write a register, call, jump) that VCSIDE writes as assembly
  static nlohmann::ordered_json actionSchema();
  std::string actionLines(const nlohmann::json& action, const std::string& indent, std::vector<std::string>* lines, std::vector<std::string>* newVariables) const;
  bool build(const nlohmann::json& answer, std::string* after, std::string* problems, int* firstLine, int* lastLine) const;

  GuidedInput input_;
  std::vector<std::string> lines_;
  std::vector<Routine> routines_;
  std::vector<std::string> variables_;  // the RAM bytes the file declares
  int variablesLine_ = 0;               // the last line of that block (0: none found)
  int stage_ = 0, attempts_ = 0;
  std::vector<std::string> recipes_;
  std::string routine_, lastAnswer_;
  mutable std::string dropped_, left_;  // of the last build: actions left out, and what the recipe leaves to do
};

}  // namespace vcs::assistant
