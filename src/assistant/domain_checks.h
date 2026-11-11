// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Checks of a proposed change that need no model: mistakes small models make again and again on the 2600 (the fire button read
// from SWCHA, names that exist nowhere). A change with such a mistake is sent back to the model, with what is wrong and the fix,
// before the user is asked to look at it (docs/design-assistant.md, "Checks before a change is shown").
#pragma once
#include <functional>
#include <string>

#include "kb.h"

namespace vcs::assistant {

// The lines `after` has and `before` has not.
std::string addedLines(const std::string& before, const std::string& after);

// Did the user ask for a new file (or folder)?
bool asksForNewFile(const std::string& request);

// What is wrong with the change (empty if nothing the checks can tell): `request` is what the user asked, `before` and `after` the file.
std::string proposalProblems(const std::string& request, const std::string& before, const std::string& after, const KnowledgeBase& kb,
                             const std::function<bool(const std::string& lowerCaseWord)>& inProject, bool batariBasic);

}  // namespace vcs::assistant
