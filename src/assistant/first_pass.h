// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The first thing asked of the model in a new chat: a short title for the tab, the language the user writes in and
// the request in English (docs/design-assistant.md, "The first pass"). The reference, the index and most models
// work in English, so what is searched for is the English text; the answer is still given in the user's language.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "assistant/provider.h"

namespace vcs::assistant {

struct FirstPass {
  bool ok = false;
  std::string language;  // "es", "en"...
  std::string title;     // at most five words, in the user's language
  std::string english;   // the request in English (as it is when it already was)
  std::string error;
};

// Reads the model's reply: a JSON object, possibly after a <think> block or inside a fence. ok only if there is a title.
FirstPass parseFirstPass(const std::string& modelText);
// A title for a tab: one line, no quotes, no final period, at most maxBytes (whole characters).
std::string cleanTitle(const std::string& text, size_t maxBytes = 44);

// The request, on a worker thread (never answers twice). The UI calls poll() every frame.
class FirstPassRequest {
 public:
  ~FirstPassRequest() { cancel(); }
  void start(const Provider& provider, const std::string& question);
  void cancel();
  bool running() const { return running_; }
  bool poll(FirstPass* result);  // true once, when the answer is there

 private:
  std::thread thread_;
  std::mutex mutex_;
  FirstPass result_;
  bool ready_ = false;
  std::atomic<bool> running_{false}, cancel_{false};
};

}  // namespace vcs::assistant
