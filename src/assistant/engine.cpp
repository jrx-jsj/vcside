// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/engine.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "assistant/http.h"
#include "paths.h"

namespace fs = std::filesystem;

namespace vcs::assistant {

namespace {

const char* kHelperName =
#ifdef _WIN32
    "llama-server.exe";
#else
    "llama-server";
#endif

fs::path helperRecords() { return fs::path(configDir()) / "llm-helpers"; }

long ownProcessId() {
#ifdef _WIN32
  return (long)GetCurrentProcessId();
#else
  return (long)getpid();
#endif
}

bool processAlive(long pid) {
  if (pid <= 0) return false;
#ifdef _WIN32
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (!process) return false;
  DWORD code = 0;
  bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
#else
  return kill((pid_t)pid, 0) == 0;
#endif
}

// Whether the process is the helper program (a process id can be reused by another program).
bool isHelperProcess(long pid) {
#ifdef _WIN32
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
  if (!process) return false;
  char path[MAX_PATH * 2];
  DWORD size = sizeof path;
  bool helper = QueryFullProcessImageNameA(process, 0, path, &size) && fs::path(path).filename().string() == kHelperName;
  CloseHandle(process);
  return helper;
#else
  std::string command = "ps -p " + std::to_string(pid) + " -o comm= 2>/dev/null";
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) return false;
  char line[1024] = {};
  bool read = fgets(line, sizeof line, pipe) != nullptr;
  pclose(pipe);
  std::string name = read ? std::string(line) : "";
  while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
  return !name.empty() && fs::path(name).filename().string() == kHelperName;
#endif
}

void endProcess(long pid) {
#ifdef _WIN32
  if (HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid)) {
    TerminateProcess(process, 1);
    CloseHandle(process);
  }
#else
  kill((pid_t)pid, SIGTERM);
#endif
}

void recordHelper(long helperPid) {
  if (helperPid <= 0) return;
  std::error_code error;
  fs::create_directories(helperRecords(), error);
  std::ofstream(helperRecords() / std::to_string(helperPid)) << ownProcessId() << "\n";
#ifndef _WIN32
  // the watchdog: while VCSIDE runs and the helper is still the helper program, wait; then
  // end the helper. It ends by itself once the helper has stopped.
  std::string script =
      "while kill -0 \"$1\" 2>/dev/null && ps -p \"$2\" -o comm= 2>/dev/null | grep -q llama-server; do sleep 2; done; "
      "if ps -p \"$2\" -o comm= 2>/dev/null | grep -q llama-server; then kill \"$2\"; fi";
  launchDetached({"/bin/sh", "-c", script, "vcside-helper-watchdog", std::to_string(ownProcessId()), std::to_string(helperPid)}, "", nullptr);
#endif
}

void forgetHelper(long helperPid) {
  if (helperPid <= 0) return;
  std::error_code error;
  fs::remove(helperRecords() / std::to_string(helperPid), error);
}

}  // namespace

void endOrphanHelpers() {
  std::error_code error;
  for (auto& entry : fs::directory_iterator(helperRecords(), error)) {
    long helperPid = std::atol(entry.path().filename().string().c_str());
    long ownerPid = 0;
    std::ifstream(entry.path()) >> ownerPid;
    if (ownerPid == ownProcessId() || processAlive(ownerPid)) continue;  // its VCSIDE still runs
    if (processAlive(helperPid) && isHelperProcess(helperPid)) endProcess(helperPid);
    std::error_code removeError;
    fs::remove(entry.path(), removeError);
  }
}

std::string helperPath() {
  if (const char* override = std::getenv("VCSIDE_LLM_HELPER_PATH"))
    return fileExists(override) ? override : "";
  const char* name = kHelperName;
  const char* base = SDL_GetBasePath();
  if (!base) return "";
  // Inside the macOS bundle the executable is in Contents/MacOS and the helper in Contents/Resources.
  for (const char* folder : {"../Resources/llm/bin/", "llm/bin/"}) {
    std::string candidate = std::string(base) + folder + name;
    if (fileExists(candidate)) return candidate;
  }
  return "";
}

std::vector<std::string> helperArguments(const std::string& modelPath, int port, int contextTokens, const std::vector<std::string>& extra) {
  std::vector<std::string> arguments = {helperPath(),
          "-m", modelPath,
          "--host", "127.0.0.1",
          "--port", std::to_string(port),
          "-c", std::to_string(contextTokens),
          "-np", "1",          // one chat at a time: all the memory goes to one context
          "--no-webui",
          "--no-warmup"};
  arguments.insert(arguments.end(), extra.begin(), extra.end());
  return arguments;
}

std::string Engine::baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1"; }

void Engine::start(const std::string& modelPath, int contextTokens, const std::vector<std::string>& extra) {
  stop();
  error_.clear();
  modelPath_ = modelPath;
  if (!available()) {
    state_ = State::Failed;
    error_ = "This build of VCSIDE does not include the language-model helper.";
    return;
  }
  if (!fileExists(modelPath)) {
    state_ = State::Failed;
    error_ = "The model file is missing: " + modelPath;
    return;
  }
  port_ = freeLoopbackPort();
  if (!port_) {
    state_ = State::Failed;
    error_ = "No free local port to start the helper on.";
    return;
  }
  job_.keepLastBytes(32 * 1024);
  if (!job_.start(helperArguments(modelPath, port_, contextTokens, extra), "")) {
    state_ = State::Failed;
    error_ = "The helper could not be started.";
    return;
  }
  recordedPid_ = job_.pid();
  recordHelper(recordedPid_);
  state_ = State::Starting;
  startedAt_ = lastCheck_ = std::chrono::steady_clock::now();
}

void Engine::stop() {
  if (state_ == State::Stopped) return;
  job_.stopAndWait();
  forgetHelper(recordedPid_);
  recordedPid_ = 0;
  if (health_.valid()) health_.wait();
  state_ = State::Stopped;
}

void Engine::poll() {
  if (state_ != State::Starting && state_ != State::Ready) return;
  if (job_.finished()) {
    // Died: on Starting it never loaded the model (too big for the memory, bad file); on Ready, it crashed.
    std::string log = job_.output();
    if (log.size() > 1500) log = log.substr(log.size() - 1500);
    error_ = state_ == State::Starting ? "The model could not be loaded.\n" + log : "The helper stopped.\n" + log;
    state_ = State::Failed;
    return;
  }
  if (state_ != State::Starting) return;
  auto now = std::chrono::steady_clock::now();
  if (health_.valid()) {
    if (health_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    if (health_.get()) {
      state_ = State::Ready;
      return;
    }
    lastCheck_ = now;
  }
  if (now - lastCheck_ < std::chrono::milliseconds(300)) return;
  lastCheck_ = now;
  std::string url = "http://127.0.0.1:" + std::to_string(port_) + "/health";
  health_ = std::async(std::launch::async, [url] {
    HttpRequest request;
    request.url = url;
    request.connectSeconds = 1;
    request.idleSeconds = 2;
    std::atomic<bool> never{false};
    return httpRequest(request, [](const char*, size_t) { return true; }, never).status == 200;
  });
}

}  // namespace vcs::assistant
