// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The assistant's user settings (kept in settings.json under "assistant") and the chats
// of a project (.vcsai/chats). No key is ever stored here: a provider names an
// environment variable (docs/design-assistant.md, "Privacy and secrets").
#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "assistant/provider.h"

namespace vcs::assistant {

struct Settings {
  bool asked = false;  // the first-start question has been answered
  bool chat = false;   // the chat panel
  // Planned features, remembered so the first-start answer is kept as given.
  bool agent = false, completion = false, explainErrors = false;
  int toolProfile = 2;  // what the agent may use: 0 read only, 1 + propose edits, 2 + build and run (each asked)
  std::vector<Provider> providers;
  int active = 0;

  bool anyFeature() const { return chat || agent || completion || explainErrors; }
  Provider* provider() { return active >= 0 && active < (int)providers.size() ? &providers[active] : nullptr; }
  nlohmann::json toJson() const;
  static Settings fromJson(const nlohmann::json& value);
};

// One saved conversation.
struct ChatMessage {
  std::string role, content;
  std::vector<std::string> reference;  // ids of the knowledge-base entries the answer was grounded on
  std::string kind;                    // "": a message; "call": the assistant asked for a tool; "result": the tool's answer
};
struct Chat {
  std::string id;      // file name without extension
  std::string title;
  std::string model, provider;
  std::vector<ChatMessage> messages;
};

// <project>/.vcsai/chats: one JSON file per chat, archived ones in archive/. Creating the
// folder also writes .vcsai/.gitignore so chats never reach git.
class ChatStore {
 public:
  explicit ChatStore(std::string projectDir) : projectDir_(std::move(projectDir)) {}
  struct Item {
    std::string id, title;
    bool archived = false;
  };
  std::vector<Item> list() const;  // newest first
  bool load(const std::string& id, bool archived, Chat* chat) const;
  bool save(const Chat& chat);
  bool archive(const std::string& id);
  bool remove(const std::string& id, bool archived);
  static std::string newId();
  std::string chatsDir() const;

 private:
  std::string projectDir_;
  bool ensureFolders() const;
};

}  // namespace vcs::assistant
