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

// One question answered whole, on the calling thread (command-line tools and tests of the procedures; the interface uses the
// classes above). schema: the answer's shape, or null. False with *error set if the server does not answer.
bool chatOnce(const Provider& provider, const std::vector<Message>& messages, const nlohmann::ordered_json* schema, int maxTokens, std::string* text, std::string* error, int idleSeconds = 120, double temperature = -1);

}  // namespace vcs::assistant
