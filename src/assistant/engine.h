// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The language model that ships with VCSIDE: a helper program (llama.cpp's server, built with
// the IDE and installed next to it) that VCSIDE starts on demand on a loopback port and stops
// when the assistant is closed. The user never sees a server (docs/design-assistant.md).
#pragma once
#include <chrono>
#include <future>
#include <string>
#include <vector>

#include "child_process.h"

namespace vcs::assistant {

// Where the helper is installed, or "" when this build has none (VCSIDE_LLM_HELPER=OFF).
// VCSIDE_LLM_HELPER_PATH overrides it, for development.
std::string helperPath();

// The helper's command line for a model (exposed for tests).
std::vector<std::string> helperArguments(const std::string& modelPath, int port, int contextTokens, const std::vector<std::string>& extra = {});

// A helper outlives VCSIDE when VCSIDE ends without stopping it (a crash, a forced quit):
// it keeps the model in memory. Each running helper is recorded in configDir()/llm-helpers
// (a file named after its process id, holding VCSIDE's), and on macOS and Linux a small
// shell watchdog ends it as soon as its VCSIDE is gone. At start-up VCSIDE ends the helpers
// still recorded whose VCSIDE no longer runs (only if they really are the helper program).
void endOrphanHelpers();

class Engine {
 public:
  enum class State { Stopped, Starting, Ready, Failed };

  ~Engine() { stop(); }
  bool available() const { return !helperPath().empty(); }
  // Starts the helper with this model; stops the previous one first. Returns at once.
  // extra: more arguments for the helper (an embedding model needs "--embedding --pooling last").
  void start(const std::string& modelPath, int contextTokens, const std::vector<std::string>& extra = {});
  void stop();
  // Call every frame: checks whether the helper has finished loading the model or has died.
  void poll();

  State state() const { return state_; }
  const std::string& error() const { return error_; }
  const std::string& modelPath() const { return modelPath_; }
  std::string baseUrl() const;       // http://127.0.0.1:<port>/v1
  std::string log() { return job_.output(); }

 private:
  Job job_;
  long recordedPid_ = 0;  // the helper's id, as recorded in llm-helpers
  State state_ = State::Stopped;
  std::string error_, modelPath_;
  int port_ = 0;
  std::chrono::steady_clock::time_point startedAt_, lastCheck_;
  std::future<bool> health_;
};

}  // namespace vcs::assistant
