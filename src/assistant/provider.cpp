// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/provider.h"

#include <cstdlib>
#include <nlohmann/json.hpp>

namespace vcs::assistant {

using json = nlohmann::json;

bool Provider::isLocal() const {
  Url url = parseUrl(baseUrl);
  return url.valid && isLoopbackHost(url.host);
}

std::string joinUrl(const std::string& base, const std::string& path) {
  std::string left = base;
  while (!left.empty() && left.back() == '/') left.pop_back();
  std::string right = path;
  while (!right.empty() && right[0] == '/') right.erase(0, 1);
  return left + "/" + right;
}

std::string openAiRequestBody(const Provider& provider, const std::vector<Message>& messages) {
  json body;
  if (!provider.model.empty()) body["model"] = provider.model;
  body["stream"] = true;
  body["stream_options"] = {{"include_usage", true}};
  body["messages"] = json::array();
  for (auto& message : messages) body["messages"].push_back({{"role", message.role}, {"content", message.content}});
  return body.dump();
}

void parseOpenAiStreamData(const std::string& data, std::vector<Event>& events) {
  if (data == "[DONE]") {
    events.push_back({Event::Done, "", 0, 0});
    return;
  }
  json value = json::parse(data, nullptr, false);
  if (!value.is_object()) return;
  if (value.contains("error")) {
    const json& error = value["error"];
    std::string message = error.is_object() ? error.value("message", std::string("error")) : error.is_string() ? error.get<std::string>() : "error";
    events.push_back({Event::Error, message, 0, 0});
    return;
  }
  if (value.contains("choices") && value["choices"].is_array() && !value["choices"].empty()) {
    const json& choice = value["choices"][0];
    if (choice.contains("delta") && choice["delta"].is_object()) {
      const json& content = choice["delta"]["content"];
      if (content.is_string() && !content.get<std::string>().empty()) events.push_back({Event::Text, content.get<std::string>(), 0, 0});
    }
  }
  if (value.contains("usage") && value["usage"].is_object()) {
    Event usage;
    usage.kind = Event::Usage;
    usage.promptTokens = value["usage"].value("prompt_tokens", 0);
    usage.completionTokens = value["usage"].value("completion_tokens", 0);
    events.push_back(usage);
  }
}

namespace {
std::vector<std::pair<std::string, std::string>> headersFor(const Provider& provider, bool json) {
  std::vector<std::pair<std::string, std::string>> headers;
  if (json) headers.push_back({"Content-Type", "application/json"});
  if (!provider.keyEnv.empty())
    if (const char* key = std::getenv(provider.keyEnv.c_str())) headers.push_back({"Authorization", std::string("Bearer ") + key});
  return headers;
}

std::string describeFailure(const HttpResult& result) {
  std::string message = result.error;
  if (message.empty()) message = "HTTP " + std::to_string(result.status);
  json body = json::parse(result.errorBody, nullptr, false);
  if (body.is_object() && body.contains("error")) {
    const json& error = body["error"];
    if (error.is_object() && error.contains("message") && error["message"].is_string()) message += ": " + error["message"].get<std::string>();
    else if (error.is_string()) message += ": " + error.get<std::string>();
  } else if (!result.errorBody.empty()) {
    message += ": " + result.errorBody.substr(0, 300);
  }
  return message;
}
}  // namespace

ChatRequest::~ChatRequest() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
}

void ChatRequest::push(Event event) {
  std::lock_guard<std::mutex> lock(mutex_);
  events_.push_back(std::move(event));
}

std::vector<Event> ChatRequest::poll() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Event> taken(events_.begin(), events_.end());
  events_.clear();
  return taken;
}

void ChatRequest::start(const Provider& provider, std::vector<Message> messages) {
  if (thread_.joinable()) {
    cancel_ = true;
    thread_.join();
  }
  cancel_ = false;
  running_ = true;
  events_.clear();
  thread_ = std::thread([this, provider, messages]() mutable { run(provider, std::move(messages)); });
}

void ChatRequest::run(Provider provider, std::vector<Message> messages) {
  HttpRequest request;
  request.method = "POST";
  request.url = joinUrl(provider.baseUrl, "chat/completions");
  request.headers = headersFor(provider, true);
  request.body = openAiRequestBody(provider, messages);
  SseParser parser;
  bool done = false;
  HttpResult result = httpRequest(
      request,
      [&](const char* bytes, size_t size) {
        for (auto& data : parser.feed(bytes, size)) {
          std::vector<Event> events;
          parseOpenAiStreamData(data, events);
          for (auto& event : events) {
            if (event.kind == Event::Done) done = true;
            push(std::move(event));
          }
        }
        return !cancel_;
      },
      cancel_);
  if (cancel_) {
    push({Event::Done, "cancelled", 0, 0});
  } else if (result.status / 100 != 2 || !result.error.empty()) {
    push({Event::Error, describeFailure(result), 0, 0});
  } else if (!done) {
    push({Event::Done, "", 0, 0});  // a server that just closes the stream
  }
  running_ = false;
}

std::vector<std::string> listModels(const Provider& provider, std::string* error) {
  std::vector<std::string> ids;
  HttpRequest request;
  request.url = joinUrl(provider.baseUrl, "models");
  request.headers = headersFor(provider, false);
  request.connectSeconds = 2;
  request.idleSeconds = 5;
  std::string body;
  std::atomic<bool> never{false};
  HttpResult result = httpRequest(request, [&](const char* bytes, size_t size) { return body.append(bytes, size), body.size() < (1 << 20); }, never);
  if (result.status / 100 != 2 || !result.error.empty()) {
    if (error) *error = describeFailure(result);
    return ids;
  }
  json value = json::parse(body, nullptr, false);
  if (value.is_object() && value.contains("data") && value["data"].is_array())
    for (auto& item : value["data"])
      if (item.is_object() && item.contains("id") && item["id"].is_string()) ids.push_back(item["id"].get<std::string>());
  return ids;
}

}  // namespace vcs::assistant

namespace vcs::assistant {
std::vector<Provider> detectLocalServers() {
  struct Known {
    const char* name;
    const char* url;
  };
  static const Known known[] = {{"Ollama", "http://127.0.0.1:11434/v1"}, {"LM Studio", "http://127.0.0.1:1234/v1"}, {"llama-server", "http://127.0.0.1:8080/v1"}};
  std::vector<Provider> found;
  for (auto& server : known) {
    Provider provider;
    provider.name = server.name;
    provider.baseUrl = server.url;
    std::string error;
    std::vector<std::string> models = listModels(provider, &error);
    if (!error.empty()) continue;
    if (!models.empty()) provider.model = models[0];
    found.push_back(provider);
  }
  return found;
}
}  // namespace vcs::assistant
