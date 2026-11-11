// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/settings.h"

#include "assistant/prompt.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace vcs::assistant {

namespace fs = std::filesystem;
using json = nlohmann::json;

json Settings::toJson() const {
  json value = {{"asked", asked}, {"chat", chat}, {"agent", agent}, {"completion", completion}, {"explain_errors", explainErrors}, {"active", active}, {"tool_profile", toolProfile}, {"budget_tokens", budgetTokens}, {"keys_in_file", keysInFile}, {"smart_search", smartSearch}, {"work_in_english", workInEnglish}, {"context_tuned", contextTuned}, {"embedding_model", embeddingModel}};
  value["providers"] = json::array();
  for (auto& provider : providers)
    value["providers"].push_back({{"name", provider.name},
                                  {"dialect", provider.dialect},
                                  {"base_url", provider.baseUrl},
                                  {"model", provider.model},
                                  {"builtin", provider.builtin},
                                  {"preset", provider.preset},
                                  {"key_env", provider.keyEnv},
                                  {"context_tokens", provider.contextTokens},
                                  {"trusted", provider.trusted}});
  return value;
}

Settings Settings::fromJson(const json& input) {
  Settings settings;
  json value = input.is_object() ? input : json::object();
  settings.asked = value.value("asked", false);
  settings.chat = value.value("chat", false);
  settings.agent = value.value("agent", false);
  settings.completion = value.value("completion", false);
  settings.explainErrors = value.value("explain_errors", false);
  settings.active = value.value("active", 0);
  settings.budgetTokens = std::max(0, value.value("budget_tokens", 0));
  settings.smartSearch = value.value("smart_search", false);
  settings.keysInFile = value.value("keys_in_file", false);
  settings.workInEnglish = value.value("work_in_english", true);
  settings.contextTuned = value.value("context_tuned", false);
  settings.embeddingModel = value.value("embedding_model", std::string("qwen3-embedding-0.6b"));
  settings.toolProfile = std::clamp(value.value("tool_profile", 2), 0, 2);
  if (value.contains("providers") && value["providers"].is_array())
    for (auto& item : value["providers"]) {
      if (!item.is_object()) continue;
      Provider provider;
      provider.name = item.value("name", std::string("Local"));
      provider.dialect = item.value("dialect", std::string("openai"));
      provider.baseUrl = item.value("base_url", std::string());
      provider.model = item.value("model", std::string());
      provider.builtin = item.value("builtin", false);
      provider.preset = item.value("preset", std::string());
      provider.keyEnv = item.value("key_env", std::string());
      provider.contextTokens = std::clamp(item.value("context_tokens", 8192), 1024, 1 << 20);
      provider.trusted = item.value("trusted", false);
      settings.providers.push_back(provider);
    }
  // The model that ships with VCSIDE is always there, first, and is the default.
  bool hasBuiltin = false;
  for (auto& provider : settings.providers) hasBuiltin = hasBuiltin || provider.builtin;
  if (!hasBuiltin) {
    Provider builtin;
    builtin.name = "Built-in";
    builtin.builtin = true;
    settings.providers.insert(settings.providers.begin(), builtin);
    settings.active = 0;  // the built-in model is the default, also for settings saved before it existed
  }
  if (settings.active < 0 || settings.active >= (int)settings.providers.size()) settings.active = 0;
  return settings;
}

std::string ChatStore::chatsDir() const { return (fs::path(projectDir_) / ".vcsai" / "chats").string(); }

bool ChatStore::ensureFolders() const {
  std::error_code error;
  fs::create_directories(fs::path(projectDir_) / ".vcsai" / "chats" / "archive", error);
  if (error) return false;
  // Conversations hold code and tool results: they must not reach git by accident, so the
  // entry is put back if someone removed it.
  fs::path ignore = fs::path(projectDir_) / ".vcsai" / ".gitignore";
  std::string text;
  if (std::ifstream in{ignore}) text.assign(std::istreambuf_iterator<char>(in), {});
  if (text.find("chats/") == std::string::npos) std::ofstream(ignore, std::ios::app) << (text.empty() || text.back() == '\n' ? "" : "\n") << "chats/\n";
  return true;
}

std::string ChatStore::newId() {
  auto now = std::chrono::system_clock::now();
  std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y%m%d-%H%M%S", std::localtime(&seconds));
  return buffer;
}

std::vector<ChatStore::Item> ChatStore::list() const {
  std::vector<Item> items;
  for (bool archived : {false, true}) {
    fs::path dir = archived ? fs::path(chatsDir()) / "archive" : fs::path(chatsDir());
    std::error_code error;
    for (auto& entry : fs::directory_iterator(dir, error)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
      Item item;
      item.id = entry.path().stem().string();
      item.archived = archived;
      json value = json::parse(std::ifstream(entry.path()), nullptr, false);
      item.title = value.is_object() ? value.value("title", std::string()) : std::string();
      items.push_back(item);
    }
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.archived != b.archived ? !a.archived : a.id > b.id; });
  return items;
}

bool ChatStore::load(const std::string& id, bool archived, Chat* chat) const {
  fs::path path = (archived ? fs::path(chatsDir()) / "archive" : fs::path(chatsDir())) / (id + ".json");
  json value = json::parse(std::ifstream(path), nullptr, false);
  if (!value.is_object()) return false;
  chat->id = id;
  chat->title = value.value("title", std::string());
  chat->model = value.value("model", std::string());
  chat->provider = value.value("provider", std::string());
  chat->messages.clear();
  if (value.contains("messages") && value["messages"].is_array())
    for (auto& item : value["messages"]) {
      if (!item.is_object()) continue;
      ChatMessage message;
      message.role = item.value("role", std::string());
      message.content = item.value("content", std::string());
      message.kind = item.value("kind", std::string());
      message.attached = item.value("attached", std::string());
      message.warnings = item.value("warnings", std::string());
      message.english = item.value("english", std::string());
      if (item.contains("reference") && item["reference"].is_array())
        for (auto& id : item["reference"])
          if (id.is_string()) message.reference.push_back(id.get<std::string>());
      chat->messages.push_back(message);
    }
  return true;
}

bool ChatStore::save(const Chat& chat) {
  if (!ensureFolders() || chat.id.empty() || chat.id.find_first_of("/\\") != std::string::npos) return false;
  json value = {{"title", sanitizeUtf8(chat.title)}, {"model", sanitizeUtf8(chat.model)}, {"provider", sanitizeUtf8(chat.provider)}};
  value["messages"] = json::array();
  for (auto& message : chat.messages) {
    json item = {{"role", message.role}, {"content", sanitizeUtf8(message.content)}};
    if (!message.reference.empty()) item["reference"] = message.reference;
    if (!message.kind.empty()) item["kind"] = message.kind;
    if (!message.english.empty()) item["english"] = sanitizeUtf8(message.english);
    if (!message.warnings.empty()) item["warnings"] = sanitizeUtf8(message.warnings);
    if (!message.attached.empty()) item["attached"] = sanitizeUtf8(message.attached);
    value["messages"].push_back(item);
  }
  // a chat that was archived stays in the archive
  fs::path path = fs::path(chatsDir()) / (chat.id + ".json");
  fs::path archived = fs::path(chatsDir()) / "archive" / (chat.id + ".json");
  std::error_code error;
  if (fs::exists(archived, error)) path = archived;
  std::ofstream(path) << value.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
  return true;
}

bool ChatStore::archive(const std::string& id) {
  if (!ensureFolders()) return false;
  std::error_code error;
  fs::rename(fs::path(chatsDir()) / (id + ".json"), fs::path(chatsDir()) / "archive" / (id + ".json"), error);
  return !error;
}

bool ChatStore::remove(const std::string& id, bool archived) {
  std::error_code error;
  fs::path path = (archived ? fs::path(chatsDir()) / "archive" : fs::path(chatsDir())) / (id + ".json");
  return fs::remove(path, error);
}

}  // namespace vcs::assistant
