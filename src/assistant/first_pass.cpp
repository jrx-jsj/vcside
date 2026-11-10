// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/first_pass.h"

#include <nlohmann/json.hpp>

#include "assistant/http.h"
#include "assistant/prompt.h"

namespace vcs::assistant {

using json = nlohmann::json;

namespace {
const char* kInstruction =
    "You prepare a user's request for an assistant. Reply with ONLY a JSON object, nothing else:\n"
    "{\"language\": \"<ISO 639-1 code of the language the user writes in>\", "
    "\"title\": \"<a title of at most 5 words, in the user's language, that names the task>\", "
    "\"english\": \"<the request translated to English; unchanged if it already is English. Keep code, register names, labels and file names exactly as written>\"}";

std::string stripThinking(std::string text) {
  size_t open = text.find("<think>");
  size_t close = text.find("</think>");
  if (open != std::string::npos && close != std::string::npos && close > open) text.erase(open, close + 8 - open);
  else if (close != std::string::npos) text.erase(0, close + 8);
  return text;
}
}  // namespace

std::string cleanTitle(const std::string& text, size_t maxBytes) {
  std::string line;
  for (char c : sanitizeUtf8(text)) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && (line.empty() || line.back() == ' ')) continue;
    line += c;
  }
  auto strip = [&](const char* marks) {
    while (!line.empty() && std::string(marks).find(line.back()) != std::string::npos) line.pop_back();
    while (!line.empty() && std::string(marks).find(line.front()) != std::string::npos) line.erase(0, 1);
  };
  strip("\"'`*# .:;,");
  if (line.size() > maxBytes) line = utf8Cut(line, maxBytes - 3) + "...";
  return line;
}

FirstPass parseFirstPass(const std::string& modelText) {
  FirstPass result;
  std::string text = stripThinking(modelText);
  size_t open = text.find('{');
  size_t close = text.rfind('}');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    result.error = "no JSON in the reply";
    return result;
  }
  json value = json::parse(text.substr(open, close - open + 1), nullptr, false);
  if (!value.is_object()) {
    result.error = "the reply is not JSON";
    return result;
  }
  auto field = [&](const char* key) { return value.contains(key) && value[key].is_string() ? value[key].get<std::string>() : std::string(); };
  result.title = cleanTitle(field("title"));
  result.english = field("english");
  result.language = field("language");
  for (char& c : result.language) c = (char)std::tolower((unsigned char)c);
  if (result.language.size() > 8) result.language.clear();
  result.ok = !result.title.empty();
  if (!result.ok) result.error = "the reply has no title";
  return result;
}

void FirstPassRequest::cancel() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void FirstPassRequest::start(const Provider& provider, const std::string& question) {
  cancel();
  cancel_ = false;
  { std::lock_guard<std::mutex> lock(mutex_); ready_ = false, result_ = FirstPass(); }
  running_ = true;
  thread_ = std::thread([this, provider, question]() {
    FirstPass result;
    // a long request is not translated whole: the title comes from its start
    std::string asked = utf8Cut(question, 1500);
    std::string body = chatRequestBody(provider, {{"system", kInstruction}, {"user", asked}});
    json request = json::parse(body, nullptr, false);
    if (request.is_object()) {
      request["stream"] = false;
      request.erase("stream_options");
      request["temperature"] = 0;
      request["max_tokens"] = 400;
      HttpRequest http;
      http.method = "POST";
      http.url = chatUrl(provider);
      http.headers = chatHeaders(provider);
      http.body = request.dump(-1, ' ', false, json::error_handler_t::replace);
      http.connectSeconds = 3;
      http.idleSeconds = 25;
      std::string answer;
      HttpResult sent = httpRequest(http, [&](const char* bytes, size_t size) { return answer.append(bytes, size), answer.size() < (1u << 20); }, cancel_);
      if (sent.status / 100 == 2 && sent.error.empty()) {
        json reply = json::parse(answer, nullptr, false);
        std::string content;
        if (reply.is_object() && reply.contains("choices") && reply["choices"].is_array() && !reply["choices"].empty() && reply["choices"][0].contains("message") &&
            reply["choices"][0]["message"].contains("content") && reply["choices"][0]["message"]["content"].is_string())
          content = reply["choices"][0]["message"]["content"].get<std::string>();
        else if (reply.is_object() && reply.contains("content") && reply["content"].is_array() && !reply["content"].empty() && reply["content"][0].contains("text") && reply["content"][0]["text"].is_string())
          content = reply["content"][0]["text"].get<std::string>();  // the Anthropic dialect
        result = parseFirstPass(content);
      } else {
        result.error = sent.error.empty() ? "HTTP " + std::to_string(sent.status) : sent.error;
      }
    }
    if (!cancel_) {
      std::lock_guard<std::mutex> lock(mutex_);
      result_ = result, ready_ = true;
    }
    running_ = false;
  });
}

bool FirstPassRequest::poll(FirstPass* result) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_) return false;
  *result = result_;
  ready_ = false;
  return true;
}

}  // namespace vcs::assistant
