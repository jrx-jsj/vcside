// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// Running external tools (assemblers, compilers, emulators) on every platform via SDL3.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcs {

// A command run in the background whose combined stdout/stderr is captured.
class Job {
 public:
  ~Job();
  bool start(const std::vector<std::string>& args, const std::string& workDir);
  bool running() const { return running_; }
  bool finished() const { return started_ && !running_; }
  int exitCode() const { return exitCode_; }
  std::string output();  // thread-safe copy

 private:
  std::thread thread_;
  std::mutex mutex_;
  std::string output_;
  std::atomic<bool> running_{false};
  bool started_ = false;
  std::atomic<int> exitCode_{-1};
};

// Starts a program and forgets it (emulators). Returns false and an error if it fails.
bool launchDetached(const std::vector<std::string>& args, const std::string& workDir, std::string* error);

// Looks for an executable in PATH. Adds ".exe" on Windows.
std::string findInPath(const std::string& name);
bool fileExists(const std::string& path);
std::string quoteArgs(const std::vector<std::string>& args);

}  // namespace vcs
