// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// What each tool takes, said once (docs/design-assistant.md, "The agent's calls"). The same table gives the schemas that go to
// a model with native tool calls, checks the arguments a model sent and repairs what is unambiguous: a model writes the path
// as "file", puts a number in a string, sends the arguments as a JSON string or flat beside the tool's name, or spells the tool
// slightly differently. What cannot be repaired comes back as a message that names each field and says how to fix it.
#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "assistant/tools.h"

namespace vcs::assistant {

// The real name of a tool from what a model wrote ("ReadFile", "read-file", "functions.read_file", "readfile"), or "".
std::string resolveToolName(const std::string& written);

struct CallCheck {
  bool ok = true;
  std::string message;             // when not ok: what is wrong and how to fix it, for the model
  std::vector<std::string> notes;  // what was repaired, for the user and the model
};
// Repairs the call in place and checks it against the tool's schema.
CallCheck repairCall(ToolCall* call);

// The tools as native tool definitions: [{"type": "function", "function": {name, description, parameters}}], or for the
// Anthropic dialect [{name, description, input_schema}].
nlohmann::json nativeTools(const std::vector<std::string>& names, bool anthropic);

}  // namespace vcs::assistant
