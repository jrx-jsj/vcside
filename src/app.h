// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// VCSIDE main window: menus, docked panels and the build/run cycle.
#pragma once
#include <SDL3/SDL.h>
#include <imgui.h>

#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "about.h"
#include "basic_syntax.h"
#include "builder_parse.h"
#include "debugmap.h"
#include "editor/text_editor.h"
#include "gfx_editor.h"
#include "kb.h"
#include "menu.h"
#include "child_process.h"
#include "cycles.h"
#include "project.h"
#include "elf_space.h"
#include "space.h"
#include "theme_ui.h"
#include "toolchain.h"

struct ImFont;

namespace vcs {

struct OpenFile {
  std::string path;
  TextEditor editor;
  std::unique_ptr<GfxEditor> gfx;  // a .vcsgfx: the graphics editor instead of the text
  bool asmSyntax = true;
  bool requestFocus = false;
  bool dirty() const { return gfx ? gfx->dirty() : editor.dirty(); }
};

class App {
 public:
  // imguiMenu: draw the menus with ImGui even where the system has a menu bar (tests)
  bool init(SDL_Window* window, ImFont* mono, bool imguiMenu = false);
  // Editor zoom: the font is rasterised again at each size so it stays sharp. main()
  // adds the font between frames, when editorFontWanted() gives a size (points).
  float editorFontWanted() const;
  void setEditorFont(float points, ImFont* font) { editorFonts_[points] = font; }
  void refreshMenus() { if (nativeMenu_) nativeMenuRefresh(); }
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
  std::string testMenuDump() { return nativeMenu_ ? nativeMenuDump() : std::string(); }
  bool testMenu(const std::string& path);  // "File/Save": the system menu, or the ImGui one
  void testGoToAddress(const std::string& a) { showGoToAddress_ = true, pendingAddress_ = a; }
  void testFocus(const std::string& panel) { ImGui::SetWindowFocus(("###" + panel).c_str()); }
  // What completion offers for a prefix and the tooltip of a word, in the active file.
  std::string testComplete(const std::string& prefix);
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
  MenuBar buildMenuBar(bool native);
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
  void missingToolDialog();
  void askForTool(const std::string& id, const std::string& why, std::function<void()> then);
  void closeDialogs();

  // actions
  void openProject(const std::string& vcsproj);
  void openFolder(const std::string& dir);
  OpenFile* openFile(const std::string& path, int line = 0);
  bool saveFile(OpenFile& f);
  // What the graphics editor checks against: a kernel of one's own for assembly, or the
  // kernel and options the main .bas sets (read again when the file changes).
  gfx::Profile gfxProfile();
  void updateBasicRegions(OpenFile& gfxFile);
  std::string gfxProfileFile_;
  std::filesystem::file_time_type gfxProfileTime_{};
  gfx::Profile gfxProfile_;
  gfx::Tv gfxTv() const;
  void saveAll();
  void build(int thenRun);  // 0 = no, 1 = run, 2 = debug
  void run(bool debug);
  void pollBuild();
  void startSpace();
  void pollSpace();
  void loadSymbols();
  std::string resolveSource(const std::string& listingName) const;  // file name in the listing -> absolute path
  // breakpoints: per file (absolute path, 0-based lines), kept in <project>.vcsproj.user
  void loadUserState();
  void saveUserState();
  std::vector<std::string> debugArgs(const std::string& emulator);  // writes the scripts, returns extra arguments
  void goToAddressDialog();
  void emulatorOptionsDialog();
  std::string findProjectFile(const std::string& name) const;
  void lint(OpenFile& f);
  void showDoc(const std::string& word);
  bool gotoDefinition(const std::string& word, const std::string& fromFile, int fromLine);
  // Where a symbol is defined: the open file first, then the project's sources.
  bool findDefinition(const std::string& word, const std::string& fromFile, int fromLine, std::string& path, int& line);

  // editor hooks
  EditorHooks asmHooks();
  EditorHooks basicHooks();
  std::string registerHelp(const Register& r) const;
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
  float editorZoom_ = 1.0f;
  std::map<float, ImFont*> editorFonts_;  // by size in points
  ImFont* editorFont() const;
  void zoomEditor(int steps);  // 0 = back to 100 %
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
  // What the sources define now, before building: the open files as they are in the
  // editor and the project's other .asm files on disk (rescanned when they change).
  struct LiveSymbol {
    SourceSymbol sym;
    std::string file;  // absolute path
  };
  struct ScannedFile {
    long long mtime = 0;
    std::vector<SourceSymbol> syms;
  };
  std::map<std::string, LiveSymbol> liveSymbols_;  // global names (not .local)
  std::map<std::string, int> liveValues_;          // the constants that evaluate
  std::map<std::string, ScannedFile> diskSymbols_;
  uint64_t liveTicks_ = 0, diskTicks_ = 0;
  // The sources of the last build (from its listing): they are scanned too, even outside
  // the folder, so a built symbol none of them defines now was deleted or renamed, and the
  // last build's table no longer offers it.
  std::vector<std::string> builtFiles_;
  bool liveReady_ = false;  // the scan includes the last build's sources
  bool builtGone(const std::string& name) const {
    return liveReady_ && !builtFiles_.empty() && !liveSymbols_.count(name);
  }
  void refreshLiveSymbols(bool force = false);
  std::string liveSymbolText(const LiveSymbol& s, bool full) const;
  AddressMap addresses_;
  std::map<std::string, std::set<int>> breakpoints_;
  std::set<std::string> treeToggled_;  // project tree folders opened or closed against their default
  bool showGoToAddress_ = false;
  bool showEmuOptions_ = false;
  std::string emuOptionsTab_;  // emulator whose tab to select when the dialog opens
  char goToAddress_[64] = "";
  std::string pendingAddress_;  // tests
  std::string lastStatus_;
  Job spaceJob_;               // second DASM run (-f2) for the space report
  bool spaceRunning_ = false;
  unsigned bottomDock_ = 0;    // dock node of the output panel (the space panel joins it)
  std::optional<SpaceReport> space_;
  std::optional<ElfSpace> elfSpace_;  // ELF projects
  void measureElf();
  bool finishBasicBuild();
  void measureBasic(long romSize);
  std::string spaceNote_;      // why there is no report (ELF, custom build...)

  // docs
  std::string docQuery_;
  std::string docTopic_;  // instruction / register / format id currently shown
  bool focusDocs_ = false;

  // dialogs
  bool showNewProject_ = false, showTools_ = false, showAbout_ = false;
  std::optional<AboutInfo> about_;  // while the About window is open (not on macOS)
  unsigned aboutTexture_ = 0;       // the icon, as an OpenGL texture
  // a tool the build or the run needs and is missing; missingThen_ retries
  std::string missingTool_, missingWhy_;
  std::function<void()> missingThen_;
  bool showMissingTool_ = false;
  char npName_[64] = "mygame";
  std::string npDir_;
  int npLang_ = 0, npTv_ = 0, npExample_ = 0;  // 0 = empty, then Project::examples()

  // settings
  std::vector<std::string> recent_;
  bool showCycles_ = true;
  bool resetLayout_ = false;
  // themes
  theme::Library themes_;
  std::string themeId_ = "dark";
  theme::Editor themeEditor_;
  void useTheme(const std::string& id, bool save = true);
  bool nativeMenu_ = false;  // menus in the system menu bar (macOS)

  std::mutex dialogMutex_;
  std::deque<std::pair<std::string, std::string>> dialogResults_;
  std::string pendingDialogKind_;
};

}  // namespace vcs
