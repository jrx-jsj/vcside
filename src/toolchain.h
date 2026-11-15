// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// External tools: where they are, and where to get them when they are missing.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace vcs {

struct Tool {
  std::string id;           // "dasm", "stella", "gopher2600", "test7800", "bb", "wasmtime", "arm-gcc", "make"
  std::string name;         // shown to the user
  std::string role;         // "assembler", "compiler", "emulator", "runtime", "build tool"
  std::string path;         // resolved executable (empty = not found)
  std::string userPath;     // fixed by the user in settings (has priority)
  std::string homepage;     // where to download it
  std::string note;         // install hint
  bool found() const { return !path.empty(); }
};

// How the tools are listed (Tools window, About): by what they do.
struct ToolGroup {
  const char* title;               // English; translated where shown
  std::vector<std::string> roles;  // Tool::role values
};

class Toolchain {
 public:
  Toolchain();
  void detect();  // re-resolves every tool
  Tool* get(const std::string& id);
  std::vector<Tool>& tools() { return tools_; }
  static const std::vector<ToolGroup>& groups();
  std::vector<Tool*> inGroup(const ToolGroup& group);

  // ~/.vcside-like folder from SDL_GetPrefPath, where tools can be dropped.
  static std::string toolsDir();

  void loadSettings(const std::map<std::string, std::string>& userPaths);
  std::map<std::string, std::string> userPaths() const;
  // Every place a tool was found (the user's path first, then where VCSIDE looks), for a
  // project that needs one with something the first one lacks.
  std::vector<std::string> foundPaths(const std::string& id) const;

 private:
  std::vector<std::string> candidates(const Tool& tool) const;
  std::vector<Tool> tools_;
};

}  // namespace vcs
