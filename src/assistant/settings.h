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
  bool nativeTools = true;           // Agent: use the service's own tool calls when it takes them (the text block is the fallback)
  bool contextTuned = false;         // the built-in model's window was raised once for a computer with the memory for it
  bool workInEnglish = true;         // translate the question to English and the answer back, code and names untouched
  bool smartSearch = false;          // search the project by meaning too (embeddings; downloads a small model)
  std::string embeddingModel = "qwen3-embedding-0.6b";
  bool keysInFile = false;  // API keys in a file only the user can read, instead of the system's secret store
  int budgetTokens = 0;  // remote models: stop asking after this many tokens in a session (0 = no limit)
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
  std::string english = {};            // what the model saw or wrote when the text was translated (content is what the user sees)
  std::string warnings = {};           // names in the answer's code that exist neither in the reference nor in the project, comma separated
  std::string attached = {};           // what went along with a question: "main.asm:12-20, Build errors"
};
struct Chat {
  std::string id;      // file name without extension
  std::string title;
  std::string model, provider;
  std::vector<ChatMessage> messages;
};

// One JSON file per chat, archived ones in archive/. Where: with a `chatsBase` (the application's own data folder) in
// <chatsBase>/<project name>-<hash of its path>/, so conversations (which hold code and tool results) never land in a project
// folder or in git; without one, as before, in <project>/.vcsai/chats (and .vcsai/.gitignore keeps them out of git).
// (Chats an older version left in a project's .vcsai/chats are not brought across: the feature is in beta.) Which chats were open in the tabs is kept too, so the next session shows them again.
class ChatStore {
 public:
  explicit ChatStore(std::string projectDir, std::string chatsBase = "") : projectDir_(std::move(projectDir)), chatsBase_(std::move(chatsBase)) {}
  struct Item {
    std::string id, title;
    bool archived = false;
  };
  struct OpenTabs {
    std::vector<std::string> ids;  // in tab order
    std::string active;
  };
  std::vector<Item> list() const;  // newest first
  bool load(const std::string& id, bool archived, Chat* chat) const;
  bool save(const Chat& chat);
  bool archive(const std::string& id);
  bool remove(const std::string& id, bool archived);
  OpenTabs loadOpenTabs() const;
  void saveOpenTabs(const OpenTabs& tabs);
  static std::string newId();
  static std::string folderKey(const std::string& projectDir);  // "osito-panda-3fa9c2d1"
  std::string chatsDir() const;

 private:
  std::string projectDir_, chatsBase_;
  bool ensureFolders() const;
};

}  // namespace vcs::assistant
