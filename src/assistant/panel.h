// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The Assistant panel, its settings window and the first-start question
// (docs/design-assistant.md, phases 1 and 2: chat grounded on the knowledge base).
#pragma once
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "assistant/engine.h"
#include "assistant/grounding.h"
#include "assistant/models.h"
#include "assistant/prompt.h"
#include "assistant/provider.h"
#include "assistant/settings.h"
#include "kb.h"

struct ImFont;

namespace vcs::assistant {

// What the panel can offer to attach, filled by the App on each frame.
struct PanelContext {
  const KnowledgeBase* kb = nullptr;
  std::string projectDir;   // "" = no project: chats are not saved
  std::string projectInfo;  // "DASM, F8, NTSC, main.asm"
  GroundingContext grounding;
  std::string fileName, fileText, selection, errors;
  bool canApply = false;
  std::function<void(const std::string& code)> apply;  // replaces the selection, or inserts at the cursor
};

class AssistantPanel {
 public:
  AssistantPanel();
  ~AssistantPanel();
  void load(const nlohmann::json& settings) { settings_ = Settings::fromJson(settings); }
  nlohmann::json save() const { return settings_.toJson(); }
  // True once after the settings changed and the App should save them.
  bool takeChanged() { bool changed = changed_; changed_ = false; return changed; }
  bool enabled() const { return settings_.chat; }
  bool needsFirstStart() const { return !settings_.asked; }

  void draw(const PanelContext& context, ImFont* mono, bool* open);  // the panel
  void dialogs();                                                     // first start, settings, apply
  void openSettings() { wantSettings_ = true; }
  void newChat();
  void closeProject();  // the project changed: forget the open chat

 private:
  Settings settings_;
  bool changed_ = false, wantSettings_ = false;
  // conversation
  Chat chat_;
  ChatRequest request_;
  std::string streaming_, error_, lastRequest_;
  std::vector<std::string> streamingReference_;
  int promptTokens_ = 0, completionTokens_ = 0, droppedTurns_ = 0;
  std::string input_;
  bool attachFile_ = false, attachSelection_ = true, attachErrors_ = true, focusInput_ = false, scrollDown_ = false;
  std::string chatProject_;
  // models and detection run on worker threads
  std::future<std::pair<std::vector<std::string>, std::string>> modelsFuture_;
  std::future<std::vector<Provider>> detectFuture_;
  std::vector<std::string> models_;
  std::string modelsError_, modelsFor_;
  bool detecting_ = false, fetchingModels_ = false;
  std::string detectNote_;
  // the built-in model: the helper that ships with VCSIDE and the model files it runs
  Engine engine_;
  Download download_;
  std::vector<CatalogModel> catalog_;
  bool catalogLoaded_ = false, drawnThisFrame_ = false;
  unsigned int copiedId_ = 0;                          // the code block whose copy icon was just pressed
  double copiedAt_ = 0;
  Hardware hardware_;                                  // what this computer offers, asked once
  std::future<std::vector<Device>> probe_;
  Recommendation recommendation_;                      // the model that fits it best
  // dialogs
  std::string applyCode_, applyWhere_;
  std::function<void(const std::string&)> applyFn_;
  bool wantApply_ = false, wantInspector_ = false, wantRemote_ = false, wantHistory_ = false;
  std::set<std::string> remoteConfirmed_;
  std::string pendingRemote_, projectDir_;

  void send(const PanelContext& context, const std::string& question);
  void poll(const PanelContext& context);
  void finishAnswer(const PanelContext& context, const std::string& note);
  void saveChat(const PanelContext& context);
  void refreshModels();
  const CatalogModel* catalogModel(const std::string& id) const;
  bool prepareBuiltin(Provider& provider);   // starts the helper when the model is there; true once it answers
  void builtinCard(Provider& provider);      // download offer, progress, loading, errors
  void modelCombo(Provider& provider, const char* label, float width);
  std::string systemPrompt() const;
  void firstStartDialog();
  void settingsDialog();
  void applyDialog();
  void inspectorDialog();
  void remoteDialog();
  void historyDialog();
  void drawMessage(const ChatMessage& message, const std::string& text, const PanelContext& context, ImFont* mono, int index);
};

}  // namespace vcs::assistant
