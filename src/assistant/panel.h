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
#include "assistant/edits.h"
#include "assistant/embeddings.h"
#include "assistant/engine.h"
#include "assistant/first_pass.h"
#include "assistant/mask.h"
#include "assistant/model_list.h"
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
  int cursorLine = 0;                         // the cursor's line, 1-based (a long file is cut around it)
  std::function<void(const std::string& word)> showDoc;  // opens a register or an instruction in the documentation
  std::function<bool(const std::string& relative, std::string* text)> readOpenFile;  // the editor's text of an open file
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
  void load(const nlohmann::json& settings);
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
  std::future<std::vector<Provider>> detectFuture_;
  ModelLists modelLists_;                              // what each service offers, asked of the service
  std::vector<KnownProvider> known_;
  bool knownLoaded_ = false;
  std::string modelFilter_;
  struct KeyMask {
    double at = -1e9;
    bool present = false;
    std::string mask;      // "...abcd": never the key
  };
  std::map<std::string, KeyMask> keyMasks_;
  bool detecting_ = false;
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
  std::string chatsBase_;  // where chats are kept: the application's data folder (empty: the project's own .vcsai folder)
  ChatStore chatStore(const std::string& projectDir) const { return ChatStore(projectDir, chatsBase_); }
  void saveOpenTabs();     // which chats are open, for the next session
  void restoreOpenTabs();  // the chats that were open when this project was left
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
  double lastBusyAt_ = 0;                              // the last moment the built-in model was needed (it is unloaded after ten quiet minutes)
  double requestStartedAt_ = 0;                        // when the question went out, for the waiting line
  std::string waitingText_;                            // what the model is doing before its first word
  float chipsTopY_ = 0;
  float chipsRowHeight_ = 0;  // as measured on the last frame (one row of chips, or two when they wrap)
  float mentionX_ = 0, mentionY_ = 0, mentionWidth_ = 0;  // where the input is, for the list above it
  void drawContextChips(const PanelContext& context);
  void modeSwitch(float width);
  void updateMention(const PanelContext& context);
  void drawMention(const PanelContext& context, float inputX, float inputY, float width);
  void pickMention(const PanelContext& context, const std::string& entry);
  // Working in English: the prose of the question goes to be translated (its code and names kept out), the model answers in
  // English, and the prose of the answer is translated back
  std::string chatLanguage_;                           // the user's language in this chat ("es"), "" while unknown
  MaskedText questionMask_;
  bool firstPassNeedsEnglish_ = false, translateThisTurn_ = false;
  OneShotRequest translation_;
  bool translating_ = false;
  int translatingIndex_ = -1;
  MaskedText answerMask_;
  void startTranslation(const PanelContext& context);
  bool busyWithModel() const { return request_.running() || firstPassPending_ || translating_; }
  FirstPassRequest firstPass_;
  bool firstPassPending_ = false;
  std::string firstPassQuestion_;
  mutable ProjectIndex index_;                                 // the project's files, searched for each question
  ImFont* monoFont_ = nullptr;
  long long spentTokens_ = 0;                          // remote tokens used this session
  int lastUsageSpent_ = 0;
  std::string keyInput_;                               // an API key being typed; never saved to a file
  std::string pendingQuestion_;
  int agentSteps_ = 0;
  std::map<std::string, int> callCounts_;
  std::string exampleFile_ = "game.asm";  // the file the prompt's example reads and edits: the one the user works on
  int openProvider_ = -1;  // the service whose settings are open in the settings window (one at a time)
  int proposalRejects_ = 0;  // times, in this message, a change was sent back to the model by the checks
  int blockRetries_ = 0;  // times, in this message, the model was sent back an edit block that does not fit the file
  bool nudged_ = false;  // the model was already reminded, in this message, to use the tools
  std::string turnText_;                               // the user's last turn as sent (with its attachments)
  // a change from a block of the chat's answer, found in the real file, waiting for Apply or Discard
  struct Placement {
    double at = -10;           // when it was last looked for (the file may change)
    bool ok = false;
    std::string problem;       // why it cannot be placed
    bool applied = false;      // the text it would write is already in the file
    EditProposal edit;
  };
  std::map<std::string, Placement> placements_;
  bool pendingEditFromChat_ = false;
  std::set<std::string> nativeOff_;                    // services that refused tool definitions: the text block is used for them
  bool noMoreTools_ = false;                           // the last turn of a message: no tools, a summary
  std::map<std::string, std::string> lastResults_;     // the answer to each call this message, to show again on a repeat
  bool smallModel() const;
  bool agentMode() const;  // Agent is chosen and the model may use it: a small model answers in Ask mode, unless its settings allow Agent anyway
  int toolSet() const;  // 1 small model, 2 large
  void finishWithSummary(const PanelContext& context, const std::string& why);
  bool nativeCallSeen_ = false;                        // this answer already holds a native call
  std::string providerKey() const;
  bool useNativeTools() const;
  nlohmann::json nativeToolList(const Provider& provider) const;  // the tool definitions for a request, empty when none go
  bool answerCut_ = false;    // the answer stopped because the window was full
  std::string notice_;        // a line after the conversation: what the last Apply did
  const Placement& placementOf(const PanelContext& context, const std::string& key, const EditBlock& block);
  void drawEditBlock(const PanelContext& context, const EditBlock& block, const std::string& key, bool finished);
  bool pendingEditActive_ = false;
  EditProposal pendingEdit_;
  std::string pendingRun_;                             // "build" or "run_emulator", waiting for a click
  std::set<std::string> allowedRun_, allowedEdit_;     // this conversation's session grants
  std::map<std::string, std::pair<bool, std::string>> originals_;  // path -> (created here, text before the first change)
  bool privilegedTouched_ = false;                     // a Makefile or project file was changed: builds ask again
  bool waitingBuild_ = false, buildSeen_ = false;
  int buildFrames_ = 0;
  // dialogs
  bool wantInspector_ = false, wantRemote_ = false, wantHistory_ = false;
  std::set<std::string> remoteConfirmed_;
  std::string pendingRemote_, projectDir_;
  bool remoteDontAsk_ = false;
  bool sendConfirmed_ = false;  // "Send to it" was pressed: the message waiting in the input goes now  // the "Don't ask again" box of the remote question
  bool remoteAllowed(const Provider& provider) const { return provider.isLocal() || provider.builtin || provider.trusted || remoteConfirmed_.count(provider.baseUrl); }

  void send(const PanelContext& context, const std::string& question);
  // Room kept in the window for the answer: a change with its diff takes a couple of thousand tokens.
  static int replyReserve(int window) { return std::clamp(window * 30 / 100, 2048, 4096); }
  void dispatch(const PanelContext& context, const std::string& question, const std::string& english);
  void poll(const PanelContext& context);
  void finishAnswer(const PanelContext& context, const std::string& note);
  void saveChat(const PanelContext& context);
  bool overBudget(const Provider& provider);
  void keyControls(Provider& provider);
  void keyStoreChoice(float wrapWidth);
  ToolEnv toolEnv(const PanelContext& context) const;
  void continueAgent(const PanelContext& context);
  void handleToolCall(const PanelContext& context, const ToolCall& call);
  void addToolResult(const PanelContext& context, const std::string& tool, const std::string& text, bool error);
  void performRun(const PanelContext& context, const std::string& tool);
  void decideEdit(const PanelContext& context, bool apply, bool allowFile);
  void undoEverything(const PanelContext& context);
  void agentCards(const PanelContext& context);
  void modelPicker(Provider& provider, const char* label, float width, bool withRefresh);
  const CatalogModel* catalogModel(const std::string& id) const;
  bool prepareBuiltin(Provider& provider);   // starts the helper when the model is there; true once it answers
  void builtinCard(Provider& provider);      // download offer, progress, loading, errors
  void modelCombo(Provider& provider, const char* label, float width);
  std::string systemPrompt() const;  // for the turn in hand: in English when the question was translated
  void firstStartDialog();
  void settingsDialog();
  void inspectorDialog();
  void remoteDialog();
  void historyDialog();
  void drawMessage(const ChatMessage& message, const std::string& text, const PanelContext& context, ImFont* mono, int index);
  void applyAllBlocks(const PanelContext& context, const std::vector<std::pair<EditBlock, std::string>>& blocks);
  void drawMessageContent(const ChatMessage& message, const std::string& text, const PanelContext& context, ImFont* mono, int index);
  int selectingIndex_ = -1;                            // the message shown as selectable text (right click > Select text)
};

}  // namespace vcs::assistant
