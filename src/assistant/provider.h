// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Talking to a model: the neutral messages and events, and the OpenAI-style chat
// completions dialect (llama.cpp's server, Ollama, LM Studio and many remote services
// speak it). See docs/design-assistant.md.
#pragma once
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "assistant/http.h"
#include "assistant/sse.h"

namespace vcs::assistant {

struct Message {
  std::string role;  // "system", "user", "assistant"
  std::string content;
};

// A provider is saved configuration, not code.
struct Provider {
  std::string name = "Local";
  std::string dialect = "openai";                 // "openai"; "anthropic" is planned
  std::string baseUrl = "http://127.0.0.1:11434/v1";
  std::string model;
  bool builtin = false;                           // the helper that ships with VCSIDE; model = catalogue id
  std::string keyEnv;                             // name of an environment variable holding the key
  int contextTokens = 8192;
  bool isLocal() const;                           // loopback host
};

// What a worker reports to the UI thread.
struct Event {
  enum Kind { Text, Usage, Error, Done } kind = Text;
  std::string text;          // Text: a piece of the answer; Error: the message
  int promptTokens = 0, completionTokens = 0;  // Usage
};

// The request body for the chat completions endpoint.
std::string openAiRequestBody(const Provider& provider, const std::vector<Message>& messages);
// Reads one streamed data payload ("[DONE]" included) into events.
void parseOpenAiStreamData(const std::string& data, std::vector<Event>& events);
// The body for the provider's dialect (what the request inspector shows: never a key).
std::string chatRequestBody(const Provider& provider, const std::vector<Message>& messages);
// The Anthropic Messages API: the system prompt is its own field, not a message.
std::string anthropicRequestBody(const Provider& provider, const std::vector<Message>& messages, int maxTokens = 4096);
void parseAnthropicStreamData(const std::string& data, std::vector<Event>& events);
// The request's address and headers for the provider's dialect, key included (looked up in
// the secret store, then in the environment variable the provider names).
std::string chatUrl(const Provider& provider);
std::vector<std::pair<std::string, std::string>> chatHeaders(const Provider& provider);
std::string keyOf(const Provider& provider);   // "" if there is none
// "http://127.0.0.1:11434/v1" + "/chat/completions", without doubling slashes.
std::string joinUrl(const std::string& base, const std::string& path);

// One streamed answer, on a worker thread. The UI thread calls poll() every frame.
class ChatRequest {
 public:
  ~ChatRequest();
  void start(const Provider& provider, std::vector<Message> messages);
  void cancel() { cancel_ = true; }
  bool running() const { return running_; }
  std::vector<Event> poll();  // takes what arrived since the last call

 private:
  void run(Provider provider, std::vector<Message> messages);
  std::thread thread_;
  std::mutex mutex_;
  std::deque<Event> events_;
  std::atomic<bool> running_{false}, cancel_{false};
  void push(Event event);
};

// The model ids a server offers (GET <base>/models); empty with *error set if it does not answer.
std::vector<std::string> listModels(const Provider& provider, std::string* error);

}  // namespace vcs::assistant

namespace vcs::assistant {
// Looks (on loopback only, short timeouts) for the servers local models usually run on:
// llama.cpp's llama-server (8080), Ollama (11434) and LM Studio (1234). One provider per
// server that answers, with its first model.
std::vector<Provider> detectLocalServers();
}  // namespace vcs::assistant
