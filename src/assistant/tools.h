// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The tools the agent can call (docs/design-assistant.md, "The tools the agent gets"):
// the sandbox every path goes through, the read tools, the proposed edit and the way a
// call is written and found in a model's answer. Nothing here touches the UI, so every
// rule is tested without a model.
#pragma once
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "assistant/grounding.h"
#include "kb.h"

namespace vcs::assistant {

// What the tools may look at. The App fills the text snapshots on each frame.
struct ToolEnv {
  std::string projectDir;
  const KnowledgeBase* kb = nullptr;
  GroundingContext grounding;
  std::string projectInfo;   // one line
  std::string buildErrors;   // the last build's diagnostics, one per line
  std::string spaceReport;   // ROM and RAM left, as the Space panel shows them
  std::string symbols;       // "name file:line" per line
  size_t readCapChars = 12000;  // the most a read gives back (smaller for a small window)
  size_t searchLimit = 40;      // the most lines a search gives back
  std::function<std::string(const std::string& query)> findCode;
  // The text of a file as the editor holds it (unsaved changes included), by its path relative to the project; false if it is not open.
  std::function<bool(const std::string& relative, std::string* text)> openFileText;  // ranked search of the project index
};

enum class Tier { Read, Propose, Run };

struct ToolCall {
  std::string name;
  nlohmann::json args = nlohmann::json::object();
};

// A change to one file that waits for the user's click.
struct EditProposal {
  std::string path;       // absolute
  std::string relative;   // as the model wrote it
  std::string before, after;
  bool created = false;
  bool privileged = false;  // changes what Build runs: no session grant covers it
  int changes = 0;
  int firstLine = 0, lastLine = 0;  // the lines of the file the change touches (1-based, in the text before; 0 for a new file)
};

struct ToolResult {
  std::string text;          // what goes back to the model
  bool error = false;
  bool proposed = false;     // an edit waiting for approval (edit field filled)
  EditProposal edit;
};

// The names the model may use, with their tier. profile: 0 read only, 1 read + edit, 2 all.
bool toolTier(const std::string& name, Tier* tier);
// compact: 0 every tool; 1 the small set for small models (read a file or a folder, search, find code, the reference, edit,
// build); 2 the medium set for large models: everything except project_info and list_files (the open project is already in the
// context and read_file lists a folder), so there are fewer detours.
std::vector<std::string> toolNames(int profile, int compact = 0);
// What a tool does, in a sentence (the text after the signature in its line).
std::string toolDescription(const std::string& name);
// The tool section of the system prompt: how to write a call, and each tool in one line.
std::string toolPrompt(int profile, bool native = false, int compact = 0);

// Finds one call in an answer: a ```tool block, a <tool_call> element or a bare JSON object
// that names a tool. `before` gets the text in front of it. False if there is none.
bool findToolCall(const std::string& answer, ToolCall* call, std::string* before = nullptr);

// The sandbox: a path the model wrote -> an absolute path inside the project, or an error.
// Rejects "..", absolute paths, links that leave the folder, files that hold secrets and
// the folders VCSIDE keeps for itself.
bool resolveInProject(const std::string& projectDir, const std::string& relative, std::string* absolute, std::string* error);
bool isSecretFile(const std::string& fileName);
// Makefile, .vcsproj and scripts: an edit changes what Build runs.
bool isBuildFile(const std::string& fileName);

// The text of a project file for the model or the context chips: through the sandbox, text only
// (no ROMs, images or files with NUL bytes), at most maxBytes (0 = the usual limit of 256 KB).
bool readProjectFile(const std::string& projectDir, const std::string& relative, std::string* text, std::string* error, size_t maxBytes = 0);

// Runs a read tool, or turns edit_file into a proposal (nothing is written). build and
// run_emulator are the panel's: they return an error here.
ToolResult runTool(const ToolEnv& env, const ToolCall& call);

// Search-and-replace: every `search` must occur exactly once in the text.
bool applySearchReplace(const std::string& text, const nlohmann::json& edits, std::string* result, std::string* error);

// A short line for the conversation: "read_file main.asm".
std::string describeCall(const ToolCall& call);

}  // namespace vcs::assistant
