// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// A short question to the model that is answered whole, not streamed (a title, a translation): on a worker
// thread, the UI polls it every frame. Works with both dialects.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "assistant/provider.h"

namespace vcs::assistant {

class OneShotRequest {
 public:
  ~OneShotRequest() { cancel(); }
  void start(const Provider& provider, const std::string& system, const std::string& user, int maxTokens, int idleSeconds = 60);
  void cancel();
  bool running() const { return running_; }
  // True once, when the answer (or the failure) is there. Thinking blocks are removed from the text.
  bool poll(std::string* text, std::string* error);

 private:
  std::thread thread_;
  std::mutex mutex_;
  std::string text_, error_;
  bool ready_ = false;
  std::atomic<bool> running_{false}, cancel_{false};
};

std::string withoutThinking(std::string text);

}  // namespace vcs::assistant
