// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The Assistant panel, its settings window and the first-start question
// (docs/design-assistant.md, phases 1 and 2: chat grounded on the knowledge base).
#pragma once
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "assistant/context_items.h"
#include "assistant/embeddings.h"
#include "assistant/engine.h"
#include "assistant/grounding.h"
#include "assistant/models.h"
#include "assistant/project_index.h"
#include "assistant/prompt.h"
#include "assistant/provider.h"
#include "assistant/settings.h"
#include "assistant/tools.h"
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
  std::string fileRelative;  // the open file, relative to the project (for the index and the chips)
  int selectionFirst = 0, selectionLast = 0;  // the selected lines, 1-based
  bool canApply = false;
  std::function<void(const std::string& code)> apply;  // replaces the selection, or inserts at the cursor
  // for the agent's tools
  std::string spaceReport, symbols;                    // snapshots, see ToolEnv
  bool building = false;                               // a build is running
  std::string buildStatus;                             // how the last build ended
  std::function<void()> startBuild, runEmulator;
  // Writes a file: into the editor buffer when it is open (one undoable step), else to disk.
  // remove = true deletes a file the assistant created (the user's "undo everything").
  std::function<bool(const std::string& path, const std::string& text, bool remove)> applyEdit;
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
  bool explainEnabled() const { return settings_.chat && settings_.explainErrors; }
  // Asks the assistant to explain an error: sent as soon as the panel is open and idle.
  void explain(const std::string& question) { pendingQuestion_ = question; }
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
  bool focusInput_ = false, scrollDown_ = false;
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
  // the agent: a call is answered by running a tool, then the model is asked again
  // open conversations, one tab each; chat_ and the agent state below are the active tab's live copy
  struct ChatTabState {
    Chat chat;
    std::map<std::string, std::pair<bool, std::string>> originals;
    std::set<std::string> allowedRun, allowedEdit;
    bool privilegedTouched = false;
    unsigned uid = 0;
  };
  std::vector<ChatTabState> tabs_;
  int activeTab_ = 0, wantTab_ = -1;
  unsigned nextTabUid_ = 1;
  void ensureTabs();
  void storeActiveTab();
  void loadTab(int index);
  void switchTab(int index);
  void addTab();
  void closeTab(int index);
  void drawTabs(bool busy);
  void openSavedChat(const Chat& loaded);
  // searching the project by meaning: a second copy of the helper with an embedding model, a worker
  // that embeds the chunks, and the vectors kept on disk
  Engine embedEngine_;
  EmbeddingWorker embedWorker_;
  EmbeddingCache embedCache_;
  Download embedDownload_;
  std::vector<CatalogModel> embedCatalog_;
  bool embedCatalogLoaded_ = false, embedWasRunning_ = false;
  std::string embedCacheFor_, embedNote_;
  double embedLastTick_ = -10;
  const CatalogModel* embedModel();
  void updateEmbeddings(const PanelContext& context);
  std::vector<float> queryVector(const std::string& text);
  void embedCard();
  // the chips above the input and the @ list
  ContextSet contextSet_;
  struct InputState {
    int cursor = 0;
    int deleteStart = -1, deleteLength = 0;  // text for the input to drop on its next frame (the "@name" just chosen)
  } inputState_;
  std::vector<std::string> mentionEntries_;
  std::string mentionQuery_, mentionNote_, mentionListFor_;
  int mentionStart_ = -1, mentionIndex_ = 0, mentionDismissed_ = -1;
  double mentionNoteAt_ = 0;
  float chipsTopY_ = 0;
  float chipsRowHeight_ = 0;  // as measured on the last frame (one row of chips, or two when they wrap)
  float mentionX_ = 0, mentionY_ = 0, mentionWidth_ = 0;  // where the input is, for the list above it
  void drawContextChips(const PanelContext& context);
  void updateMention(const PanelContext& context);
  void drawMention(const PanelContext& context, float inputX, float inputY, float width);
  void pickMention(const PanelContext& context, const std::string& entry);
  mutable ProjectIndex index_;                                 // the project's files, searched for each question
  ImFont* monoFont_ = nullptr;
  long long spentTokens_ = 0;                          // remote tokens used this session
  int lastUsageSpent_ = 0;
  std::string keyInput_;                               // an API key being typed; never saved to a file
  std::string pendingQuestion_;
  int agentSteps_ = 0;
  std::map<std::string, int> callCounts_;
  std::string turnText_;                               // the user's last turn as sent (with its attachments)
  bool pendingEditActive_ = false;
  EditProposal pendingEdit_;
  std::string pendingRun_;                             // "build" or "run_emulator", waiting for a click
  std::set<std::string> allowedRun_, allowedEdit_;     // this conversation's session grants
  std::map<std::string, std::pair<bool, std::string>> originals_;  // path -> (created here, text before the first change)
  bool privilegedTouched_ = false;                     // a Makefile or project file was changed: builds ask again
  bool waitingBuild_ = false, buildSeen_ = false;
  int buildFrames_ = 0;
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
  bool overBudget(const Provider& provider);
  void keyControls(Provider& provider);
  ToolEnv toolEnv(const PanelContext& context) const;
  void continueAgent(const PanelContext& context);
  void handleToolCall(const PanelContext& context, const ToolCall& call);
  void addToolResult(const PanelContext& context, const std::string& tool, const std::string& text, bool error);
  void performRun(const PanelContext& context, const std::string& tool);
  void decideEdit(const PanelContext& context, bool apply, bool allowFile);
  void undoEverything(const PanelContext& context);
  void agentCards(const PanelContext& context);
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
