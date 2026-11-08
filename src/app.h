// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// VCSIDE main window: menus, docked panels and the build/run cycle.
#pragma once
#include <SDL3/SDL.h>
#include <imgui.h>

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "builder_parse.h"
#include "editor/text_editor.h"
#include "kb.h"
#include "process.h"
#include "project.h"
#include "space.h"
#include "toolchain.h"

struct ImFont;

namespace vcs {

struct OpenFile {
  std::string path;
  TextEditor editor;
  bool asmSyntax = true;
  bool requestFocus = false;
};

class App {
 public:
  bool init(SDL_Window* window, ImFont* mono);
  void frame();  // draws one frame of UI
  bool wantsQuit() const { return quit_; }
  void requestQuit();
  void shutdown();
  void openPath(const std::string& path);  // project file, folder or source file

  // Scripted actions for automatic tests (--build, --select, --doc).
  void testBuild() { build(0); }
  bool testBusy() const { return building_ || spaceRunning_; }
  void testSelect(int firstLine, int lastLine);
  void testDoc(const std::string& word) { showDoc(word); }
  void testFocus(const std::string& panel) { ImGui::SetWindowFocus(("###" + panel).c_str()); }
  void testDefinition(const std::string& w) {
    if (activeFile_ >= 0) gotoDefinition(w, files_[activeFile_]->path, files_[activeFile_]->editor.cursor().line);
  }
  void testFind(const std::string& q, bool replace) {
    if (activeFile_ < 0) return;
    auto& ed = files_[activeFile_]->editor;
    ed.openFind(replace);
    ed.setFindQuery(q);
    ed.findNext();
  }

 private:
  // panels
  void menuBar();
  void dockspace();
  void projectPanel();
  void editorsPanel();
  void outputPanel();
  void problemsPanel();
  void spacePanel();
  void docsPanel();
  void statusBar();
  void newProjectDialog();
  void toolsDialog();
  void aboutDialog();
  void closeDialogs();

  // actions
  void openProject(const std::string& vcsproj);
  void openFolder(const std::string& dir);
  OpenFile* openFile(const std::string& path, int line = 0);
  bool saveFile(OpenFile& f);
  void saveAll();
  void build(int thenRun);  // 0 = no, 1 = run, 2 = debug
  void run(bool debug);
  void pollBuild();
  void startSpace();
  void pollSpace();
  void loadSymbols();
  void lint(OpenFile& f);
  void showDoc(const std::string& word);
  bool gotoDefinition(const std::string& word, const std::string& fromFile, int fromLine);

  // editor hooks
  EditorHooks asmHooks();
  std::vector<Token> tokenizeAsm(const std::string& line) const;
  std::optional<int> lookupSymbol(const std::string& name) const;

  // settings
  void loadSettings();
  void saveSettings();
  void addRecent(const std::string& path);

  // file dialogs (SDL3 calls back on another thread)
  void pushDialogResult(const std::string& kind, const std::string& path);
  static void SDLCALL dialogCallback(void* userdata, const char* const* files, int filter);

  SDL_Window* window_ = nullptr;
  ImFont* mono_ = nullptr;
  bool quit_ = false, quitConfirm_ = false;
  KnowledgeBase kb_;
  std::string kbError_;
  Toolchain tools_;
  std::optional<Project> project_;
  std::string folder_;  // folder shown in the project panel
  std::vector<std::unique_ptr<OpenFile>> files_;
  int activeFile_ = -1;
  int closeRequest_ = -1;

  // build state
  Job job_;
  bool building_ = false;
  int runAfterBuild_ = 0;
  std::string output_;
  bool outputScroll_ = false;
  std::vector<Diagnostic> diagnostics_;
  std::map<std::string, int> symbols_;
  std::string lastStatus_;
  Job spaceJob_;               // second DASM run (-f2) for the space report
  bool spaceRunning_ = false;
  unsigned bottomDock_ = 0;    // dock node of the output panel (the space panel joins it)
  std::optional<SpaceReport> space_;
  std::string spaceNote_;      // why there is no report (ELF, custom build...)

  // docs
  std::string docQuery_;
  std::string docTopic_;  // instruction / register / format id currently shown
  bool focusDocs_ = false;

  // dialogs
  bool showNewProject_ = false, showTools_ = false, showAbout_ = false;
  char npName_[64] = "mygame";
  std::string npDir_;
  int npLang_ = 0, npTv_ = 0;

  // settings
  std::vector<std::string> recent_;
  bool showCycles_ = true;
  bool resetLayout_ = false;

  std::mutex dialogMutex_;
  std::deque<std::pair<std::string, std::string>> dialogResults_;
  std::string pendingDialogKind_;
};

}  // namespace vcs
