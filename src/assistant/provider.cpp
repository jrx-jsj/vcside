// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/provider.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "assistant/secrets.h"
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

std::string openAiRequestBody(const Provider& provider, const std::vector<Message>& messages, const nlohmann::json* tools) {
  json body;
  if (!provider.model.empty()) body["model"] = provider.model;
  body["stream"] = true;
  // The bundled helper runs a model that may "think" first; for a fast answer it is told not to.
  if (provider.builtin) body["chat_template_kwargs"] = {{"enable_thinking", false}};
  body["stream_options"] = {{"include_usage", true}};
  body["messages"] = json::array();
  for (auto& message : messages) body["messages"].push_back({{"role", message.role}, {"content", message.content}});
  if (tools && tools->is_array() && !tools->empty()) body["tools"] = *tools;
  return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string anthropicRequestBody(const Provider& provider, const std::vector<Message>& messages, int maxTokens, const nlohmann::json* tools) {
  json body;
  body["model"] = provider.model;
  body["max_tokens"] = maxTokens;
  body["stream"] = true;
  std::string system;
  body["messages"] = json::array();
  for (auto& message : messages) {
    if (message.role == "system") system += (system.empty() ? "" : "\n\n") + message.content;
    else body["messages"].push_back({{"role", message.role}, {"content", message.content}});
  }
  if (!system.empty()) body["system"] = system;
  if (tools && tools->is_array() && !tools->empty()) body["tools"] = *tools;
  return body.dump(-1, ' ', false, json::error_handler_t::replace);
}

namespace {
// Prompt caching for Claude models: every request of an agent run repeats the system prompt, the attached files and all the
// tool results before it, and a service charges a fraction of the price for text it has just seen. A breakpoint on the system
// text and one on the last message make the whole earlier conversation a cache hit on the next request.
void markCacheBreak(json& content) {
  if (content.is_string()) {
    std::string text = content.get<std::string>();
    content = json::array({json{{"type", "text"}, {"text", text}, {"cache_control", {{"type", "ephemeral"}}}}});
  } else if (content.is_array() && !content.empty() && content.back().is_object()) {
    content.back()["cache_control"] = {{"type", "ephemeral"}};
  }
}

std::string withCacheBreaks(const std::string& body, bool anthropicDialect) {
  json request = json::parse(body, nullptr, false);
  if (!request.is_object() || !request.contains("messages") || !request["messages"].is_array() || request["messages"].empty()) return body;
  json& messages = request["messages"];
  if (anthropicDialect) {
    if (request.contains("system")) markCacheBreak(request["system"]);
  } else if (messages[0].is_object() && messages[0].value("role", std::string()) == "system" && messages[0].contains("content")) {
    markCacheBreak(messages[0]["content"]);
  }
  json& last = messages.back();
  if (last.is_object() && last.contains("content") && !last["content"].is_null() && messages.size() > 1) markCacheBreak(last["content"]);
  return request.dump();
}

bool cachesClaude(const Provider& provider) {
  if (provider.dialect == "anthropic") return true;
  return provider.baseUrl.find("openrouter.ai") != std::string::npos && provider.model.rfind("anthropic/", 0) == 0;
}
}  // namespace

std::string chatRequestBody(const Provider& provider, const std::vector<Message>& messages, const nlohmann::json* tools) {
  std::string body = provider.dialect == "anthropic" ? anthropicRequestBody(provider, messages, 4096, tools) : openAiRequestBody(provider, messages, tools);
  return cachesClaude(provider) ? withCacheBreaks(body, provider.dialect == "anthropic") : body;
}

void ToolCallAssembler::addOpenAi(const json& toolCalls) {
  if (!toolCalls.is_array()) return;
  for (auto& item : toolCalls) {
    if (!item.is_object()) continue;
    // an index when the service gives one (llama.cpp, OpenAI); Google's layer may give none: then a new call is one with a name
    int index = item.contains("index") && item["index"].is_number_integer() ? item["index"].get<int>() : (item.contains("function") && item["function"].contains("name") ? nextIndex_++ : std::max(0, nextIndex_ - 1));
    Partial& partial = open_[index];
    if (item.contains("function") && item["function"].is_object()) {
      const json& function = item["function"];
      if (function.contains("name") && function["name"].is_string()) partial.name = function["name"].get<std::string>();
      if (function.contains("arguments")) {
        if (function["arguments"].is_string()) partial.arguments += function["arguments"].get<std::string>();
        else if (function["arguments"].is_object()) partial.arguments = function["arguments"].dump();
      }
    }
    nextIndex_ = std::max(nextIndex_, index + 1);
  }
}

void ToolCallAssembler::startAnthropic(int index, const std::string& name) { open_[index].name = name; }
void ToolCallAssembler::addAnthropic(int index, const std::string& partialJson) { open_[index].arguments += partialJson; }

void ToolCallAssembler::finishAnthropic(int index, std::vector<Event>& events) {
  auto found = open_.find(index);
  if (found == open_.end()) return;
  if (!found->second.name.empty()) events.push_back({Event::ToolCall, found->second.name, 0, 0, found->second.arguments.empty() ? "{}" : found->second.arguments});
  open_.erase(found);
}

void ToolCallAssembler::finish(std::vector<Event>& events) {
  for (auto& item : open_)
    if (!item.second.name.empty()) events.push_back({Event::ToolCall, item.second.name, 0, 0, item.second.arguments.empty() ? "{}" : item.second.arguments});
  open_.clear();
}

void parseAnthropicStreamData(const std::string& data, std::vector<Event>& events, ToolCallAssembler* tools) {
  json value = json::parse(data, nullptr, false);
  if (!value.is_object()) return;
  std::string type = value.value("type", std::string());
  if (tools && type == "content_block_start" && value.contains("content_block") && value["content_block"].is_object() && value["content_block"].value("type", std::string()) == "tool_use") {
    tools->startAnthropic(value.value("index", 0), value["content_block"].value("name", std::string()));
  } else if (tools && type == "content_block_stop") {
    tools->finishAnthropic(value.value("index", 0), events);
  } else if (type == "content_block_delta" && value.contains("delta") && value["delta"].is_object()) {
    const json& delta = value["delta"];
    if (tools && delta.value("type", std::string()) == "input_json_delta" && delta.contains("partial_json") && delta["partial_json"].is_string()) tools->addAnthropic(value.value("index", 0), delta["partial_json"].get<std::string>());
    if (delta.value("type", std::string()) == "text_delta" && delta.contains("text") && delta["text"].is_string() && !delta["text"].get<std::string>().empty())
      events.push_back({Event::Text, delta["text"].get<std::string>(), 0, 0});
  } else if (type == "message_start" && value.contains("message") && value["message"].is_object() && value["message"].contains("usage")) {
    Event usage;
    usage.kind = Event::Usage;
    usage.promptTokens = value["message"]["usage"].value("input_tokens", 0);
    usage.completionTokens = value["message"]["usage"].value("output_tokens", 0);
    events.push_back(usage);
  } else if (type == "message_delta" && value.contains("usage") && value["usage"].is_object()) {
    Event usage;
    usage.kind = Event::Usage;
    usage.promptTokens = value["usage"].value("input_tokens", 0);
    usage.completionTokens = value["usage"].value("output_tokens", 0);
    events.push_back(usage);
    if (value.contains("delta") && value["delta"].is_object() && value["delta"].value("stop_reason", std::string()) == "max_tokens") events.push_back({Event::Length, "", 0, 0});
  } else if (type == "message_stop") {
    events.push_back({Event::Done, "", 0, 0});
  } else if (type == "error") {
    std::string message = value.contains("error") && value["error"].is_object() ? value["error"].value("message", std::string("error")) : "error";
    events.push_back({Event::Error, message, 0, 0});
  }
}

std::string keyOf(const Provider& provider) {
  std::string key;
  if (loadSecret(provider.name, &key) && !key.empty()) return key;
  if (!provider.keyEnv.empty())
    if (const char* fromEnvironment = std::getenv(provider.keyEnv.c_str())) return fromEnvironment;
  return "";
}

std::string chatUrl(const Provider& provider) { return joinUrl(provider.baseUrl, provider.dialect == "anthropic" ? "messages" : "chat/completions"); }

std::vector<std::pair<std::string, std::string>> chatHeaders(const Provider& provider) {
  std::vector<std::pair<std::string, std::string>> headers = {{"Content-Type", "application/json"}};
  std::string key = keyOf(provider);
  if (provider.dialect == "anthropic") {
    headers.push_back({"anthropic-version", "2023-06-01"});
    if (!key.empty()) headers.push_back({"x-api-key", key});
  } else if (!key.empty()) {
    headers.push_back({"Authorization", "Bearer " + key});
  }
  return headers;
}

void parseOpenAiStreamData(const std::string& data, std::vector<Event>& events, ToolCallAssembler* tools) {
  if (data == "[DONE]") {
    if (tools) tools->finish(events);
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
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string() && choice["finish_reason"].get<std::string>() == "length") events.push_back({Event::Length, "", 0, 0});
    if (tools && choice.contains("delta") && choice["delta"].is_object() && choice["delta"].contains("tool_calls")) tools->addOpenAi(choice["delta"]["tool_calls"]);
    if (tools && choice.contains("finish_reason") && choice["finish_reason"].is_string() && choice["finish_reason"].get<std::string>() != "length") tools->finish(events);  // the calls are whole
    if (choice.contains("delta") && choice["delta"].is_object()) {
      // find, not [] (on a const json, [] with a missing key is undefined: a delta may be {})
      auto content = choice["delta"].find("content");
      if (content != choice["delta"].end() && content->is_string() && !content->get<std::string>().empty())
        events.push_back({Event::Text, content->get<std::string>(), 0, 0});
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
  auto headers = chatHeaders(provider);
  if (!json) headers.erase(headers.begin());  // no body: no content type
  return headers;
}

std::string describeFailure(const HttpResult& result) {
  std::string message = result.error;
  if (message.empty()) message = "HTTP " + std::to_string(result.status);
  json body = json::parse(result.errorBody, nullptr, false);
  if (body.is_array() && !body.empty()) body = body[0];  // Google's services answer an error as [{"error": {...}}]
  if (body.is_object() && body.contains("error")) {
    const json& error = body["error"];
    if (error.is_object() && error.contains("message") && error["message"].is_string()) message += ": " + error["message"].get<std::string>();
    else if (error.is_string()) message += ": " + error.get<std::string>();
  } else if (!result.errorBody.empty()) {
    message += ": " + result.errorBody.substr(0, 300);
  }
  return message;
}

// "28s", "28.9s", "250ms": a number and its unit; 0 when it is not one.
double secondsIn(const std::string& text, size_t from) {
  char* end = nullptr;
  double value = std::strtod(text.c_str() + from, &end);
  if (end == text.c_str() + from || value < 0) return 0;
  return std::strncmp(end, "ms", 2) == 0 ? value / 1000.0 : value;
}
}  // namespace

double retryDelaySeconds(const HttpResult& result) {
  if (result.retryAfterSeconds > 0) return result.retryAfterSeconds;
  json body = json::parse(result.errorBody, nullptr, false);
  if (body.is_array() && !body.empty()) body = body[0];
  if (body.is_object() && body.contains("error") && body["error"].is_object() && body["error"].contains("details") && body["error"]["details"].is_array())
    for (auto& detail : body["error"]["details"])
      if (detail.is_object() && detail.contains("retryDelay") && detail["retryDelay"].is_string()) {
        double seconds = secondsIn(detail["retryDelay"].get<std::string>(), 0);
        if (seconds > 0) return seconds;
      }
  for (const char* phrase : {"retry in ", "try again in "}) {
    size_t at = result.errorBody.find(phrase);
    if (at != std::string::npos) {
      double seconds = secondsIn(result.errorBody, at + std::strlen(phrase));
      if (seconds > 0) return seconds;
    }
  }
  return 0;
}

double pauseBeforeRetry(const HttpResult& result, int attempt) {
  static const double pauses[] = {2, 5, 10};
  static const double longestPause = 120;
  bool busy = result.status == 429 || result.status == 500 || result.status == 502 || result.status == 503 || result.status == 504;
  if (!busy || attempt < 0 || attempt >= 3) return -1;
  double asked = retryDelaySeconds(result);
  if (asked > longestPause) return -1;
  return asked > 0 ? asked + 1 : pauses[attempt];  // a second more than asked: the clocks differ
}

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

void ChatRequest::start(const Provider& provider, std::vector<Message> messages, const nlohmann::json* tools) {
  if (thread_.joinable()) {
    cancel_ = true;
    thread_.join();
  }
  cancel_ = false;
  running_ = true;
  events_.clear();
  nlohmann::json toolList = tools ? *tools : nlohmann::json::array();
  thread_ = std::thread([this, provider, messages, toolList]() mutable { run(provider, std::move(messages), std::move(toolList)); });
}

void ChatRequest::run(Provider provider, std::vector<Message> messages, nlohmann::json tools) {
  HttpRequest request;
  request.method = "POST";
  request.url = chatUrl(provider);
  request.headers = headersFor(provider, true);
  bool anthropic = provider.dialect == "anthropic";
  bool withTools = tools.is_array() && !tools.empty();
  request.body = chatRequestBody(provider, messages, withTools ? &tools : nullptr);
  // A busy service (429, 500, 502, 503, 504) before it has said anything is tried again, up to three times, after a pause: "This
  // model is currently experiencing high demand" is usually over in seconds. When the service says how long to wait (Retry-After,
  // Google's retryDelay) that is the pause (pauseBeforeRetry).
  HttpResult result;
  bool done = false;
  for (int attempt = 0;; attempt++) {
    SseParser parser;
    ToolCallAssembler assembler;
    bool delivered = false;
    done = false;
    result = httpRequest(
        request,
        [&](const char* bytes, size_t size) {
          for (auto& data : parser.feed(bytes, size)) {
            std::vector<Event> events;
            if (anthropic) parseAnthropicStreamData(data, events, withTools ? &assembler : nullptr);
            else parseOpenAiStreamData(data, events, withTools ? &assembler : nullptr);
            for (auto& event : events) {
              if (event.kind == Event::Done) done = true;
              delivered = true;
              push(std::move(event));
            }
          }
          return !cancel_;
        },
        cancel_);
    // a service that does not take tool definitions (or this model): the same request again without them, and the panel is told
    if (withTools && !cancel_ && !delivered && (result.status == 400 || result.status == 404 || result.status == 422 || result.status == 501) &&
        (result.errorBody.find("tool") != std::string::npos || result.errorBody.find("function") != std::string::npos)) {
      withTools = false;
      request.body = chatRequestBody(provider, messages, nullptr);
      push({Event::NoNativeTools, "", 0, 0});
      attempt--;
      continue;
    }
    double pause = delivered || cancel_ ? -1 : pauseBeforeRetry(result, attempt);
    if (pause < 0) break;
    push({Event::Retry, "", result.status, (int)std::ceil(pause)});
    for (int waited = 0; waited < pause * 10 && !cancel_; waited++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (cancel_) break;
    push({Event::Retry, "", result.status, 0});  // the pause is over: the panel says the new try is under way
  }
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
  // A server on this computer answers at once. A service on the internet may take several seconds for its first connection
  // (the first HTTPS request of a run took 4 s to Google): 5 s timed it out and the list never came.
  bool nearby = provider.isLocal();
  request.connectSeconds = nearby ? 2 : 15;
  request.idleSeconds = nearby ? 5 : 30;
  std::string body;
  std::atomic<bool> never{false};
  HttpResult result = httpRequest(request, [&](const char* bytes, size_t size) { return body.append(bytes, size), body.size() < (1 << 20); }, never);
  if (double pause = pauseBeforeRetry(result, 0); pause >= 0 && pause <= 15) {  // busy: once more, if the wait is short
    std::this_thread::sleep_for(std::chrono::milliseconds((int)(pause * 1000)));
    body.clear();
    result = httpRequest(request, [&](const char* bytes, size_t size) { return body.append(bytes, size), body.size() < (1 << 20); }, never);
  }
  if (result.status / 100 != 2 || !result.error.empty()) {
    if (error) *error = describeFailure(result);
    return ids;
  }
  json value = json::parse(body, nullptr, false);
  if (!value.is_object() && !value.is_array()) {
    if (error) *error = "the service answered something that is not a list of models";
    return ids;
  }
  // {"data": [{"id": ...}]} (OpenAI and the services that copy it), {"models": [{"id" or "name": ...}]} (Google's own), or a bare list
  const json* list = nullptr;
  if (value.is_array()) list = &value;
  else if (value.contains("data") && value["data"].is_array()) list = &value["data"];
  else if (value.contains("models") && value["models"].is_array()) list = &value["models"];
  if (list)
    for (auto& item : *list) {
      if (item.is_string()) ids.push_back(item.get<std::string>());
      else if (item.is_object() && item.contains("id") && item["id"].is_string()) ids.push_back(item["id"].get<std::string>());
      else if (item.is_object() && item.contains("name") && item["name"].is_string()) ids.push_back(item["name"].get<std::string>());
    }
  return ids;
}

}  // namespace vcs::assistant

namespace vcs::assistant {
std::vector<Provider> detectLocalServers() {
  struct Known {
    const char* name;
    const char* url;
  };
  static const Known known[] = {{"Ollama", "http://127.0.0.1:11434/v1"}, {"LM Studio", "http://127.0.0.1:1234/v1"}, {"llama-server", "http://127.0.0.1:8080/v1"},
                                {"llama-server (9931)", "http://127.0.0.1:9931/v1"}};  // 9931: llama.cpp's announced new default port (release b10240)
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

namespace vcs::assistant {
std::vector<KnownProvider> loadKnownProviders(const std::string& jsonPath) {
  std::vector<KnownProvider> list;
  std::ifstream input(jsonPath);
  json root = json::parse(input, nullptr, false);
  if (!root.is_object() || !root.contains("providers") || !root["providers"].is_array()) return list;
  auto text = [](const json& item, const char* key) { return item.contains(key) && item[key].is_string() ? item[key].get<std::string>() : std::string(); };
  for (auto& item : root["providers"]) {
    if (!item.is_object()) continue;
    KnownProvider known;
    known.id = text(item, "id"), known.name = text(item, "name"), known.dialect = text(item, "dialect"), known.baseUrl = text(item, "base_url");
    known.keyPage = text(item, "key_page"), known.note = text(item, "note");
    known.contextTokens = item.value("context_tokens", 32768);
    // only https services, and a protocol that exists
    if (known.id.empty() || known.name.empty() || known.baseUrl.rfind("https://", 0) != 0 || (known.dialect != "openai" && known.dialect != "anthropic")) continue;
    if (!known.keyPage.empty() && known.keyPage.rfind("https://", 0) != 0) known.keyPage.clear();
    list.push_back(known);
  }
  return list;
}

Provider providerFrom(const KnownProvider& known) {
  Provider provider;
  provider.name = known.name;
  provider.dialect = known.dialect;
  provider.baseUrl = known.baseUrl;
  provider.preset = known.id;
  provider.contextTokens = known.contextTokens;
  return provider;
}
}  // namespace vcs::assistant
