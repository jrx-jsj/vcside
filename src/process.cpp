// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "process.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace vcs {

static SDL_Process* create(const std::vector<std::string>& args, const std::string& workDir, bool pipe,
                           const EnvVars& vars = {}) {
  std::vector<const char*> argv;
  for (auto& a : args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
#ifdef SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING
  if (!workDir.empty()) SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, workDir.c_str());
#else
  (void)workDir;
#endif
  SDL_Environment* env = nullptr;
  if (!vars.empty()) {
    env = SDL_CreateEnvironment(true);  // a copy of ours
    for (auto& [k, v] : vars) SDL_SetEnvironmentVariable(env, k.c_str(), v.c_str(), true);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
  }
  if (pipe) {
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
  } else {
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
  }
  SDL_Process* p = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (env) SDL_DestroyEnvironment(env);
  return p;
}

Job::~Job() {
  if (thread_.joinable()) thread_.join();
}

bool Job::start(const std::vector<std::string>& args, const std::string& workDir) {
  if (running_) return false;
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ = "> " + quoteArgs(args) + "\n";
  }
  SDL_Process* p = create(args, workDir, true);
  started_ = true;
  if (!p) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ += std::string("Could not start the process: ") + SDL_GetError() + "\n";
    exitCode_ = -1;
    return false;
  }
  running_ = true;
  thread_ = std::thread([this, p]() {
    SDL_IOStream* out = SDL_GetProcessOutput(p);
    char buf[4096];
    for (;;) {
      size_t n = out ? SDL_ReadIO(out, buf, sizeof buf) : 0;
      if (n > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        output_.append(buf, n);
      } else if (!out || SDL_GetIOStatus(out) != SDL_IO_STATUS_NOT_READY) {
        break;
      } else {
        SDL_Delay(5);
      }
    }
    int code = -1;
    SDL_WaitProcess(p, true, &code);
    SDL_DestroyProcess(p);
    exitCode_ = code;
    running_ = false;
  });
  return true;
}

std::string Job::output() {
  std::lock_guard<std::mutex> lock(mutex_);
  return output_;
}

bool launchDetached(const std::vector<std::string>& args, const std::string& workDir, std::string* error,
                    const EnvVars& env) {
  SDL_Process* p = create(args, workDir, false, env);
  if (!p) {
    if (error) *error = SDL_GetError();
    return false;
  }
  SDL_DestroyProcess(p);  // does not kill it
  return true;
}

bool fileExists(const std::string& path) {
  std::error_code ec;
  return !path.empty() && fs::exists(path, ec);
}

std::string findInPath(const std::string& name) {
  const char* path = std::getenv("PATH");
  if (!path) return {};
#ifdef _WIN32
  const char sep = ';';
  std::vector<std::string> exts = {".exe", ".bat", ".cmd", ""};
#else
  const char sep = ':';
  std::vector<std::string> exts = {""};
#endif
  std::string p = path;
  size_t start = 0;
  while (start <= p.size()) {
    size_t end = p.find(sep, start);
    if (end == std::string::npos) end = p.size();
    std::string dir = p.substr(start, end - start);
    if (!dir.empty())
      for (auto& e : exts) {
        fs::path f = fs::path(dir) / (name + e);
        std::error_code ec;
        if (fs::is_regular_file(f, ec)) return f.string();
      }
    start = end + 1;
  }
  return {};
}

std::string quoteArgs(const std::vector<std::string>& args) {
  std::string s;
  for (auto& a : args) {
    if (!s.empty()) s += ' ';
    if (a.find_first_of(" \t\"") != std::string::npos) s += "\"" + a + "\"";
    else s += a;
  }
  return s;
}

}  // namespace vcs
