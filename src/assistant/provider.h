// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Talking to a model: the neutral messages and events, and the OpenAI-style chat
// completions dialect (llama.cpp's server, Ollama, LM Studio and many remote services
// speak it). See docs/design-assistant.md.
#pragma once
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

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
  std::string preset;                             // the id of the known service it was made from ("gemini"), "" for any other
  bool builtin = false;                           // the helper that ships with VCSIDE; model = catalogue id
  std::string keyEnv;                             // name of an environment variable holding the key
  int contextTokens = 8192;
  bool trusted = false;                           // a remote server: send to it without asking each session
  bool isLocal() const;                           // loopback host
};

// What a worker reports to the UI thread.
struct Event {
  enum Kind { Text, Usage, Error, Done, Length, Retry, ToolCall, NoNativeTools } kind = Text;  // ToolCall: the model called a tool through the API (text = its name, arguments = the JSON); NoNativeTools: the service refused the tool definitions and the request went again without them  // Retry: the service was busy; promptTokens = the HTTP status, completionTokens = seconds until the next try  // Length: the answer stopped because the window was full
  std::string text;          // Text: a piece of the answer; Error: the message
  int promptTokens = 0, completionTokens = 0;  // Usage
  std::string arguments = {};
};

// The request body for the chat completions endpoint.
// tools: native tool definitions (tool_schema.h nativeTools) to send with the request, or nullptr.
std::string openAiRequestBody(const Provider& provider, const std::vector<Message>& messages, const nlohmann::json* tools = nullptr);
// Reads one streamed data payload ("[DONE]" included) into events.
// Tool calls arrive in pieces (a name, then the arguments a few characters at a time); this puts them together and says, once a
// call is whole, ToolCall.
class ToolCallAssembler {
 public:
  void addOpenAi(const nlohmann::json& toolCalls);                         // delta.tool_calls of a chunk
  void startAnthropic(int index, const std::string& name);                  // content_block_start of a tool_use
  void addAnthropic(int index, const std::string& partialJson);            // input_json_delta
  void finish(std::vector<Event>& events);                                  // every call still open becomes a ToolCall event
  void finishAnthropic(int index, std::vector<Event>& events);
 private:
  struct Partial { std::string name, arguments; };
  std::map<int, Partial> open_;
  int nextIndex_ = 0;
};
void parseOpenAiStreamData(const std::string& data, std::vector<Event>& events, ToolCallAssembler* tools = nullptr);
// The body for the provider's dialect (what the request inspector shows: never a key).
std::string chatRequestBody(const Provider& provider, const std::vector<Message>& messages, const nlohmann::json* tools = nullptr);
// The Anthropic Messages API: the system prompt is its own field, not a message.
std::string anthropicRequestBody(const Provider& provider, const std::vector<Message>& messages, int maxTokens = 4096, const nlohmann::json* tools = nullptr);
void parseAnthropicStreamData(const std::string& data, std::vector<Event>& events, ToolCallAssembler* tools = nullptr);
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
  void start(const Provider& provider, std::vector<Message> messages, const nlohmann::json* tools = nullptr);
  void cancel() { cancel_ = true; }
  bool running() const { return running_; }
  std::vector<Event> poll();  // takes what arrived since the last call

 private:
  void run(Provider provider, std::vector<Message> messages, nlohmann::json tools);
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
// A service VCSIDE knows (data/assistant/providers.json): choosing it fills the address and the protocol.
struct KnownProvider {
  std::string id, name, dialect, baseUrl, keyPage, note;
  int contextTokens = 32768;
};
std::vector<KnownProvider> loadKnownProviders(const std::string& jsonPath);
Provider providerFrom(const KnownProvider& known);

// Looks (on loopback only, short timeouts) for the servers local models usually run on:
// llama.cpp's llama-server (8080), Ollama (11434) and LM Studio (1234). One provider per
// server that answers, with its first model.
std::vector<Provider> detectLocalServers();
}  // namespace vcs::assistant
