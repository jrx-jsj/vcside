// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/engine.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdlib>

#include "assistant/http.h"

namespace vcs::assistant {

std::string helperPath() {
  if (const char* override = std::getenv("VCSIDE_LLM_HELPER_PATH"))
    return fileExists(override) ? override : "";
#ifdef _WIN32
  const char* name = "llama-server.exe";
#else
  const char* name = "llama-server";
#endif
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
  state_ = State::Starting;
  startedAt_ = lastCheck_ = std::chrono::steady_clock::now();
}

void Engine::stop() {
  if (state_ == State::Stopped) return;
  job_.stopAndWait();
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
