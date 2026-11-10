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
#include "assistant/panel.h"
#include "basic_syntax.h"
#include "builder_parse.h"
#include "debugmap.h"
#include "editor/text_editor.h"
#include "gfx_editor.h"
#include "music/music_editor.h"
#include "kb.h"
#include "menu.h"
#include "child_process.h"
#include "cycles.h"
#include "project.h"
#include "project_form.h"
#include "elf_space.h"
#include "image_viewer.h"
#include "space.h"
#include "theme_ui.h"
#include "toolchain.h"

struct ImFont;

namespace vcs {

struct OpenFile {
  std::string path;
  TextEditor editor;
  std::unique_ptr<GfxEditor> gfx;  // a .vcsgfx: the graphics editor instead of the text
  std::unique_ptr<music::MusicEditor> music;  // a .vcsmusic: the music editor
  std::unique_ptr<ImageViewer> image;  // a PNG or BMP: shown, not edited
  std::unique_ptr<ProjectForm> form;   // a .vcsproj: a form, or its text in the editor
  bool asmSyntax = true;
  bool requestFocus = false;
  bool dirty() const { return gfx ? gfx->dirty() : music ? music->dirty() : form ? form->dirty(editor) : editor.dirty(); }
};

class App;

// What an App asks of the program that holds its windows (main.cpp).
struct WindowHooks {
  std::function<void()> newWindow;                         // an empty window, on the welcome page
  std::function<bool(const std::string& folder, App* from)> raiseIfOpen;  // already in another window: bring it up
  std::function<void()> quitAll;                           // File → Quit: every window
};

class App {
 public:
  static WindowHooks hooks;
  // imguiMenu: draw the menus with ImGui even where the system has a menu bar (tests)
  bool init(SDL_Window* window, ImFont* mono, bool imguiMenu = false);
  // Zoom of the whole IDE: the fonts are rasterised again at each size so they stay
  // sharp. main() rebuilds them between frames when zoomWanted() gives a new zoom.
  float zoomWanted() const { return zoom_ != builtZoom_ ? zoom_ : 0; }
  void setFonts(ImFont* mono, float zoom) { mono_ = mono, builtZoom_ = zoom; }
  float zoom() const { return zoom_; }
  void refreshMenus() { if (nativeMenu_) nativeMenuRefresh(); }
  void frame();  // draws one frame of UI
  bool wantsQuit() const { return quit_; }
  void requestQuit();       // this window: asks about unsaved files, then wantsQuit()
  void cancelQuit() { quit_ = false; }
  void closeProject();      // back to the welcome page (asks about unsaved files)
  bool hasProject() const { return !folder_.empty() || !files_.empty(); }
  const std::string& folder() const { return folder_; }
  // What a window reopens with next time: the project file, else the folder ("" = welcome).
  std::string sessionPath() const;
  SDL_Window* window() const { return window_; }
  void setActive(bool active) { active_ = active; }  // the focused window: the system menu acts on it
  void syncSettings();      // another window saved the settings: take the shared ones
  MenuBar menuBarForSystem() { return buildMenuBar(true); }
  void showAbout() { showAbout_ = true; }
  void shutdown();
  void openPath(const std::string& path);  // project file, folder or source file

  // Scripted actions for automatic tests (--build, --select, --doc).
  void testBuild() { build(0); }
  bool testBusy() const { return building_ || spaceRunning_; }
  void testSelect(int firstLine, int lastLine);
  void testDoc(const std::string& word) { showDoc(word); }
  std::string testMenuDump() { return nativeMenu_ ? nativeMenuDump() : std::string(); }
  bool testMenu(const std::string& path);  // "File/Save": the system menu, or the ImGui one
  void testGoToAddress(const std::string& address) { showGoToAddress_ = true, pendingAddress_ = address; }
  void testFocus(const std::string& panel) { ImGui::SetWindowFocus(("###" + panel).c_str()); }
  // What completion offers for a prefix and the tooltip of a word, in the active file.
  std::string testComplete(const std::string& prefix);
  void testDefinition(const std::string& word) {
    if (activeFile_ >= 0) gotoDefinition(word, files_[activeFile_]->path, files_[activeFile_]->editor.cursor().line);
  }
  void testFind(const std::string& query, bool replace) {
    if (activeFile_ < 0) return;
    auto& textEditor = files_[activeFile_]->editor;
    textEditor.openFind(replace);
    textEditor.setFindQuery(query);
    textEditor.findNext();
  }

  void zoomEditor(int steps);  // 0 = back to 100 %

 private:
  // panels
  void menuBar();
  MenuBar buildMenuBar(bool native);
  void dockspace();
  void projectPanel();
  void welcome();
  void welcomeScreen();
  unsigned iconTexture();
  void editorsPanel();
  void outputPanel();
  void problemsPanel();
  void spacePanel();
  void docsPanel();
  void statusBar();
  void newProjectDialog();
  void toolsDialog();
  // F7/F5 in a folder without a .vcsproj that Project::detect recognises: build it as found,
  // with or without writing the project file.
  void noProjectDialog();
  bool createProjectFile();  // saves project_ (detected, not saved yet) and opens it as a project
  void aboutDialog();
  void missingToolDialog();
  void askForTool(const std::string& id, const std::string& why, std::function<void()> then);
  void closeDialogs();

  // actions
  void openProject(const std::string& vcsproj);
  // A file a source names (include "x"), looked for next to it, in the project and its
  // include folders; "" if not there or not something to open (a ROM).
  std::string linkedFile(const std::string& name, const std::string& fromFile) const;
  void projectSettings();     // the project's .vcsproj in a tab, as a form
  void projectFileSaved();    // the .vcsproj changed: the project takes it again
  void openFolder(const std::string& dir);
  OpenFile* openFile(const std::string& path, int line = 0);
  std::string importTiaTrackerFile(const std::string& path);
  bool saveFile(OpenFile& file);
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
  // From the last build's listing: what clicking a line's cycles shows (its address) and
  // the rules where a new 6502 page starts.
  std::string addressInfo(const std::string& path, int line) const;
  void updatePageSeparators(OpenFile& file);
  // breakpoints: per file (absolute path, 0-based lines), kept in <project>.vcsproj.user
  void loadUserState();
  void saveUserState();
  // The files open in the project when it was last closed: path, cursor (0-based); and the one in front.
  std::vector<std::pair<std::string, Coord>> savedFiles_;
  std::string savedActiveFile_;
  std::vector<std::string> debugArgs(const std::string& emulator);  // writes the scripts, returns extra arguments
  void goToAddressDialog();
  void emulatorOptionsDialog();
  std::string findProjectFile(const std::string& name) const;
  void lint(OpenFile& file);
  void showDoc(const std::string& word);
  bool gotoDefinition(const std::string& word, const std::string& fromFile, int fromLine);
  // Where a symbol is defined: the open file first, then the project's sources.
  bool findDefinition(const std::string& word, const std::string& fromFile, int fromLine, std::string& path, int& line);

  // editor hooks
  EditorHooks asmHooks();
  EditorHooks basicHooks();
  std::string registerHelp(const Register& reg) const;
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
  float zoom_ = 1.0f, builtZoom_ = 1.0f;
  ImFont* editorFont() const;
  bool quit_ = false, quitConfirm_ = false;
  bool closeProjectAfterConfirm_ = false;  // the "unsaved changes" dialog closes the project, not the window
  bool active_ = true;
  int settingsSeen_ = 0;
  void doCloseProject();
  KnowledgeBase kb_;
  std::string kbError_;
  Toolchain tools_;
  std::optional<Project> project_;
  std::string folder_;  // folder shown in the project panel
  std::string detectedFor_;          // the folder detected_ was looked for in
  std::optional<Project> detected_;  // how that folder builds, when it has no project
  std::optional<Project> buildCandidate_;  // what the no-project dialog offers to build
  int candidateRun_ = 0;                   // and what to do after (as build's thenRun)
  bool showNoProject_ = false;
  bool projectUnsaved_ = false;  // project_ is a folder's detection, with no .vcsproj written yet
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
    SourceSymbol symbol;
    std::string file;  // absolute path
  };
  struct ScannedFile {
    long long mtime = 0;
    std::vector<SourceSymbol> symbols;
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
  std::string liveSymbolText(const LiveSymbol& liveSymbol, bool full) const;
  AddressMap addresses_;
  std::map<std::string, std::set<int>> breakpoints_;
  std::set<std::string> treeToggled_;  // project tree folders opened or closed against their default
  // What Git ignores in the folder (shown dimmed in the tree), asked again every few seconds.
  std::set<std::string> gitIgnored_;  // absolute paths, folders without the final separator
  std::unique_ptr<Job> gitJob_;
  std::string gitFolder_;
  double gitAskedAt_ = -100;
  void updateGitIgnored();
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

  // the optional AI assistant (docs/design-assistant.md)
  assistant::AssistantPanel assistant_;
  bool showAssistant_ = true;
  void assistantPanel();

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
  int npLang_ = 0, npExample_ = 0;
  std::string npTvId_ = "NTSC";  // 0 = empty, then Project::examples()
  std::string npElfCartridge_ = "chameleon";  // ELF: the cartridge, and its sizes in KB
  int npFlashKb_ = 128, npRamKb_ = 36;

  // settings
  std::vector<std::string> recent_;
  bool showCycles_ = true;
  bool showPages_ = true;  // 6502 page separators in assembly files
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
