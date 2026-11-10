// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The first thing asked of the model about a question: a short title for the tab (first message of a chat), the language
// the user writes in and the request in English (docs/design-assistant.md, "Working in English"). The code and names in the
// question are replaced by markers before it goes (mask.h), so only the prose is translated.
#pragma once
#include <string>

#include "assistant/one_shot.h"

namespace vcs::assistant {

struct FirstPass {
  bool ok = false;
  std::string language;  // "es", "en"...
  std::string title;     // at most five words, in the user's language; empty when it was not asked for
  std::string english;   // the request in English (as it is when it already was); empty when it was not asked for
  std::string error;
};

// Reads the model's reply: a JSON object, possibly after a <think> block or inside a fence. ok if it has what was asked for.
FirstPass parseFirstPass(const std::string& modelText, bool needTitle = true, bool needEnglish = false);
// A title for a tab: one line, no quotes, no final period, at most maxBytes (whole characters).
std::string cleanTitle(const std::string& text, size_t maxBytes = 44);

class FirstPassRequest {
 public:
  void start(const Provider& provider, const std::string& maskedQuestion, bool needTitle, bool needEnglish);
  void cancel() { request_.cancel(); }
  bool running() const { return request_.running(); }
  bool poll(FirstPass* result);  // true once, when the answer is there

 private:
  OneShotRequest request_;
  bool needTitle_ = true, needEnglish_ = false;
};

// Translating an answer back: the system prompt for a language, and the name of a language from its ISO code.
std::string translationPrompt(const std::string& languageName, const std::string& from);
std::string languageName(const std::string& isoCode);

}  // namespace vcs::assistant
