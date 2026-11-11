// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/one_shot.h"

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "assistant/http.h"
#include "assistant/prompt.h"

namespace vcs::assistant {

using json = nlohmann::json;

std::string withoutThinking(std::string text) {
  size_t open = text.find("<think>");
  size_t close = text.find("</think>");
  if (open != std::string::npos && close != std::string::npos && close > open) text.erase(open, close + 8 - open);
  else if (close != std::string::npos) text.erase(0, close + 8);
  return text;
}

void OneShotRequest::cancel() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void OneShotRequest::start(const Provider& provider, const std::string& system, const std::string& user, int maxTokens, int idleSeconds) {
  cancel();
  cancel_ = false;
  { std::lock_guard<std::mutex> lock(mutex_); ready_ = false, text_.clear(), error_.clear(); }
  running_ = true;
  thread_ = std::thread([this, provider, system, user, maxTokens, idleSeconds]() {
    std::string text, error;
    json request = json::parse(chatRequestBody(provider, {{"system", system}, {"user", user}}), nullptr, false);
    if (request.is_object()) {
      request["stream"] = false;
      request.erase("stream_options");
      request["temperature"] = 0;
      request["max_tokens"] = maxTokens;
      HttpRequest http;
      http.method = "POST";
      http.url = chatUrl(provider);
      http.headers = chatHeaders(provider);
      http.body = request.dump(-1, ' ', false, json::error_handler_t::replace);
      http.connectSeconds = 3;
      http.idleSeconds = idleSeconds;
      std::string answer;
      HttpResult sent;
      for (int attempt = 0;; attempt++) {  // a busy service (429, 5xx) is tried again after the pause it asked for
        answer.clear();
        sent = httpRequest(http, [&](const char* bytes, size_t size) { return answer.append(bytes, size), answer.size() < (4u << 20); }, cancel_);
        double pause = cancel_ ? -1 : pauseBeforeRetry(sent, attempt);
        if (pause < 0) break;
        for (int waited = 0; waited < pause * 10 && !cancel_; waited++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (cancel_) break;
      }
      if (sent.status / 100 == 2 && sent.error.empty()) {
        json reply = json::parse(answer, nullptr, false);
        if (reply.is_object() && reply.contains("choices") && reply["choices"].is_array() && !reply["choices"].empty() && reply["choices"][0].contains("message") &&
            reply["choices"][0]["message"].contains("content") && reply["choices"][0]["message"]["content"].is_string())
          text = reply["choices"][0]["message"]["content"].get<std::string>();
        else if (reply.is_object() && reply.contains("content") && reply["content"].is_array() && !reply["content"].empty() && reply["content"][0].contains("text") && reply["content"][0]["text"].is_string())
          text = reply["content"][0]["text"].get<std::string>();  // the Anthropic dialect
        else error = "the server answered something unexpected";
      } else {
        error = sent.error.empty() ? "HTTP " + std::to_string(sent.status) : sent.error;
      }
    } else {
      error = "no request";
    }
    if (!cancel_) {
      std::lock_guard<std::mutex> lock(mutex_);
      text_ = withoutThinking(sanitizeUtf8(text)), error_ = error, ready_ = true;
    }
    running_ = false;
  });
}

bool OneShotRequest::poll(std::string* text, std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_) return false;
  *text = text_, *error = error_;
  ready_ = false;
  return true;
}

}  // namespace vcs::assistant
