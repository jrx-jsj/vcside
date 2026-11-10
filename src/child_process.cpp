// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "child_process.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace vcs {

static SDL_Process* create(const std::vector<std::string>& args, const std::string& workDir, bool pipe,
                           const EnvVars& vars = {}) {
  std::vector<const char*> argv;
  for (auto& arg : args) argv.push_back(arg.c_str());
  argv.push_back(nullptr);
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
#if defined(_WIN32) && defined(SDL_PROP_PROCESS_CREATE_CMDLINE_STRING)
  // SDL quotes every argument, which cmd /c does not read ("echo" is not a command). cmd
  // gets its own line instead: cmd.exe /c ""C:\a b\x.bat" "main.bas"" (cmd drops the
  // outer quotes and keeps the rest as it is).
  std::string cmdline;
  if (args.size() >= 3) {
    auto lower = [](std::string text) {
      for (auto& character : text) character = (char)std::tolower((unsigned char)character);
      return text;
    };
    std::string prog = lower(fs::path(args[0]).filename().string());
    if ((prog == "cmd" || prog == "cmd.exe") && lower(args[1]) == "/c") {
      cmdline = "cmd.exe /c \"";
      for (size_t i = 2; i < args.size(); i++) {
        bool needsQuotes = args[i].empty() || args[i].find_first_of(" \t&()^|<>,;=") != std::string::npos;
        cmdline += (i > 2 ? " " : "") + (needsQuotes ? "\"" + args[i] + "\"" : args[i]);
      }
      cmdline += "\"";
      SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_CMDLINE_STRING, cmdline.c_str());
    }
  }
#endif
#ifdef SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING
  if (!workDir.empty()) SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, workDir.c_str());
#else
  (void)workDir;
#endif
  SDL_Environment* environment = nullptr;
  if (!vars.empty()) {
    environment = SDL_CreateEnvironment(true);  // a copy of ours
    for (auto& [name, value] : vars) SDL_SetEnvironmentVariable(environment, name.c_str(), value.c_str(), true);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, environment);
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
  SDL_Process* process = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (environment) SDL_DestroyEnvironment(environment);
  return process;
}

Job::~Job() {
  stop();
  if (thread_.joinable()) thread_.join();
}

bool Job::start(const std::vector<std::string>& args, const std::string& workDir, const EnvVars& environment) {
  if (running_) return false;
  if (thread_.joinable()) thread_.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ = "> " + quoteArgs(args) + "\n";
  }
  SDL_Process* process = create(args, workDir, true, environment);
  started_ = true;
  if (!process) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_ += std::string("Could not start the process: ") + SDL_GetError() + "\n";
    exitCode_ = -1;
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    process_ = process;
  }
  pid_ = (long)SDL_GetNumberProperty(SDL_GetProcessProperties(process), SDL_PROP_PROCESS_PID_NUMBER, 0);
  running_ = true;
  thread_ = std::thread([this, process]() {
    SDL_IOStream* stream = SDL_GetProcessOutput(process);
    char buffer[4096];
    for (;;) {
      size_t bytesRead = stream ? SDL_ReadIO(stream, buffer, sizeof buffer) : 0;
      if (bytesRead > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        output_.append(buffer, bytesRead);
        if (outputLimit_ && output_.size() > outputLimit_ * 2) output_.erase(0, output_.size() - outputLimit_);
      } else if (!stream || SDL_GetIOStatus(stream) != SDL_IO_STATUS_NOT_READY) {
        break;
      } else {
        SDL_Delay(5);
      }
    }
    int code = -1;
    SDL_WaitProcess(process, true, &code);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      process_ = nullptr;
      SDL_DestroyProcess(process);
    }
    exitCode_ = code;
    pid_ = 0;
    running_ = false;
  });
  return true;
}

void Job::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (process_) SDL_KillProcess(static_cast<SDL_Process*>(process_), true);
}

void Job::stopAndWait() {
  stop();
  if (thread_.joinable()) thread_.join();
}

std::string Job::output() {
  std::lock_guard<std::mutex> lock(mutex_);
  return output_;
}

bool launchDetached(const std::vector<std::string>& args, const std::string& workDir, std::string* error,
                    const EnvVars& environment) {
  SDL_Process* process = create(args, workDir, false, environment);
  if (!process) {
    if (error) *error = SDL_GetError();
    return false;
  }
  SDL_DestroyProcess(process);  // does not kill it
  return true;
}

bool fileExists(const std::string& path) {
  std::error_code errorCode;
  return !path.empty() && fs::exists(path, errorCode);
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
  std::string pathVariable = path;
  size_t start = 0;
  while (start <= pathVariable.size()) {
    size_t end = pathVariable.find(sep, start);
    if (end == std::string::npos) end = pathVariable.size();
    std::string dir = pathVariable.substr(start, end - start);
    if (!dir.empty())
      for (auto& extension : exts) {
        fs::path candidate = fs::path(dir) / (name + extension);
        std::error_code errorCode;
        if (fs::is_regular_file(candidate, errorCode)) return candidate.string();
      }
    start = end + 1;
  }
  return {};
}

std::string quoteArgs(const std::vector<std::string>& args) {
  std::string joined;
  for (auto& arg : args) {
    if (!joined.empty()) joined += ' ';
    if (arg.find_first_of(" \t\"") != std::string::npos) joined += "\"" + arg + "\"";
    else joined += arg;
  }
  return joined;
}

}  // namespace vcs
