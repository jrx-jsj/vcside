// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include <SDL3/SDL_opengl.h>

#include "about.h"
#include "file_operations.h"
#include "folder_dialog.h"
#include "assistant/markdown_view.h"
#include "doc_markdown.h"
#include "file_icons.h"
#include "cycles.h"
#include "window_icon.h"
#include "emulator_launch.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "paths.h"
#include "music/import_tiatracker.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace vcs {

static const int kLayoutVersion = 2;  // bump when a docked panel is added
static const char* kLangIds[] = {"dasm", "batari-basic", "elf-c", "ace-c"};
static const char* kTvMultipleHelp = N_("The program runs on more than one system and handles it in its code: the DASM template chooses with -DPAL=1, batari Basic with set tv, C detects it when it starts. The emulator is not told one: it detects it from the frame.");
static const char* kLangNames[] = {N_("Assembler (DASM)"), N_("batari Basic"), N_("C for ELF (vcsLib)"), N_("C for ACE (PlusCart, UnoCart)")};

// The language's name for people, as the New project dialog shows it.
static std::string languageName(Language language) {
  for (size_t i = 0; i < std::size(kLangIds); i++)
    if (languageFromId(kLangIds[i]) == language) return T(kLangNames[i]);
  return languageId(language);
}

static std::string readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

static bool isAsmFile(const std::string& path) {
  std::string extension = upper(fs::path(path).extension().string());
  return extension == ".ASM" || extension == ".S" || extension == ".H" || extension == ".INC" || extension == ".A" || extension == ".65S";
}

static std::string lower(std::string text) {
  for (auto& character : text) character = (char)std::tolower((unsigned char)character);
  return text;
}

// ---------------------------------------------------------------------------
// Setup

WindowHooks App::hooks;
static int gSettingsSaved = 0;  // bumped on every save, so the other windows reload

bool App::init(SDL_Window* window, ImFont* mono, bool imguiMenu) {
  window_ = window;
  mono_ = mono;
  if (!kb_.load(dataDir() + "/kb", &kbError_)) SDL_Log("Knowledge base: %s", kbError_.c_str());
  loadSettings();
  tools_.detect();
  nativeMenu_ = nativeMenuAvailable() && !imguiMenu;  // main() installs it, for whichever window is focused
  settingsSeen_ = gSettingsSaved;
  return true;
}

void App::shutdown() {
  saveUserState();
  saveSettings();
}

void App::loadSettings() {
  json settings;
  try {
    std::ifstream file(configDir() + "settings.json");
    if (file) settings = json::parse(file);
  } catch (...) {
  }
  if (!settings.is_object()) settings = json::object();
  loadLanguages(dataDir() + "/i18n");
  std::string code = settings.value("language", "");
  setLanguage(code.empty() ? systemLanguage() : code);
  recent_ = settings.value("recent", std::vector<std::string>{});
  themes_.load(dataDir() + "/themes", configDir() + "themes");
  useTheme(settings.value("theme", std::string("dark")), false);
  showCycles_ = settings.value("show_cycles", true);
  columnAddresses_ = settings.value("cycle_column_addresses", false);
  showPages_ = settings.value("show_pages", true);
  projectsDir_ = settings.value("projects_dir", std::string());
  zoom_ = std::clamp(settings.value("zoom", settings.value("editor_zoom", 1.0f)), 0.5f, 3.0f);
  tools_.loadSettings(settings.value("tools", std::map<std::string, std::string>{}));
  assistant_.load(settings.value("assistant", nlohmann::json::object()));
  // A new docked panel needs the default layout once (imgui.ini would keep it floating).
  if (settings.value("layout_version", 0) < kLayoutVersion) {
    resetLayout_ = true;
    saveSettings();
  }
}

void App::saveSettings() {
  json settings = {{"language", currentLanguage()},
            {"recent", recent_},
            {"show_cycles", showCycles_},
            {"cycle_column_addresses", columnAddresses_},
            {"show_pages", showPages_},
            {"projects_dir", projectsDir_},
            {"zoom", zoom_},
            {"theme", themeId_},
            {"tools", tools_.userPaths()},
            {"assistant", assistant_.save()},
            {"layout_version", kLayoutVersion}};
  std::ofstream(configDir() + "settings.json") << settings.dump(2) << "\n";
  settingsSeen_ = ++gSettingsSaved;
}

void App::syncSettings() {
  if (settingsSeen_ == gSettingsSaved) return;
  settingsSeen_ = gSettingsSaved;
  json settings;
  try {
    std::ifstream file(configDir() + "settings.json");
    if (file) settings = json::parse(file);
  } catch (...) {
  }
  if (!settings.is_object()) return;
  recent_ = settings.value("recent", recent_);
  showCycles_ = settings.value("show_cycles", showCycles_);
  columnAddresses_ = settings.value("cycle_column_addresses", columnAddresses_);
  showPages_ = settings.value("show_pages", showPages_);
  projectsDir_ = settings.value("projects_dir", projectsDir_);
  zoom_ = std::clamp(settings.value("zoom", zoom_), 0.5f, 3.0f);
  themes_.load(dataDir() + "/themes", configDir() + "themes");
  useTheme(settings.value("theme", themeId_), false);  // this window's ImGui style
  tools_.loadSettings(settings.value("tools", std::map<std::string, std::string>{}));
  for (auto& file : files_) file->editor.setGutterWidthChars(gutterChars(*file));
  for (auto& file : files_) updatePageSeparators(*file);
}

void App::useTheme(const std::string& id, bool save) {
  themeId_ = themes_.find(id) ? id : "dark";
  theme::Resolved resolved = themes_.resolve(themeId_);
  for (auto& problem : resolved.problems) SDL_Log("Theme %s: %s", themeId_.c_str(), problem.c_str());
  theme::setCurrent(resolved);
  theme::applyImGui(resolved);
  if (save) saveSettings();
}

void App::addRecent(const std::string& path) {
  recent_.erase(std::remove(recent_.begin(), recent_.end(), path), recent_.end());
  recent_.insert(recent_.begin(), path);
  if (recent_.size() > 10) recent_.resize(10);
  saveSettings();
}

// ---------------------------------------------------------------------------
// File dialogs

struct DialogCtx {
  App* app;
  std::string kind;
};

void SDLCALL App::dialogCallback(void* userdata, const char* const* files, int) {
  auto* dialogContext = static_cast<DialogCtx*>(userdata);
  if (files && files[0]) dialogContext->app->pushDialogResult(dialogContext->kind, files[0]);
  delete dialogContext;
}

void App::pushDialogResult(const std::string& kind, const std::string& path) {
  std::lock_guard<std::mutex> lock(dialogMutex_);
  dialogResults_.emplace_back(kind, path);
}

// ---------------------------------------------------------------------------
// Projects and files

void App::openPath(const std::string& path) {
  std::error_code errorCode;
  if (fs::is_directory(path, errorCode)) {
    for (auto& entry : fs::directory_iterator(path, errorCode))
      if (entry.path().extension() == ".vcsproj") return openProject(entry.path().string());
    openFolder(path);
  } else if (fs::path(path).extension() == ".vcsproj") {
    openProject(path);
  } else if (fs::exists(path, errorCode)) {
    openFile(path);
  }
}

// A TIATracker song (.ttt) as a .vcsmusic next to it (a new name if that one is taken);
// what the import could not keep goes to the output. Empty if it cannot be read.
std::string App::importTiaTrackerFile(const std::string& path) {
  music::Song song;
  music::ImportResult result = music::importTiaTracker(readFile(path), fs::path(path).stem().string(), &song);
  if (!result.ok) {
    output_ += F("Cannot import {0}: {1}", {path, result.error}) + "\n";
    return {};
  }
  fs::path folder = fs::path(path).parent_path(), musicPath = folder / (fs::path(path).stem().string() + ".vcsmusic");
  std::error_code errorCode;
  for (int number = 2; fs::exists(musicPath, errorCode); number++)
    musicPath = folder / (fs::path(path).stem().string() + " " + std::to_string(number) + ".vcsmusic");
  std::ofstream out(musicPath, std::ios::binary);
  if (!(out << music::toJson(song))) {
    output_ += F("Cannot write {0}", {musicPath.string()}) + "\n";
    return {};
  }
  output_ += F("TIATracker song imported as {0}.", {musicPath.string()}) + "\n";
  for (auto& warning : result.warnings) output_ += "  " + warning + "\n";
  return musicPath.string();
}

std::string App::linkedFile(const std::string& name, const std::string& fromFile) const {
  if (name.empty()) return {};
  std::vector<fs::path> searchPaths;
  fs::path namePath(name);
  if (namePath.is_absolute()) searchPaths.push_back(namePath);
  else {
    searchPaths.push_back(fs::path(fromFile).parent_path() / namePath);
    if (!folder_.empty()) searchPaths.push_back(fs::path(folder_) / namePath);
    if (project_)
      for (auto& includeDir : project_->includeDirs) searchPaths.push_back(fs::path(project_->abs(includeDir)) / namePath);
    if (!folder_.empty()) searchPaths.push_back(fs::path(folder_) / "include" / namePath);
  }
  std::error_code errorCode;
  for (auto& candidate : searchPaths)
    if (fs::is_regular_file(candidate, errorCode) && files::kindOf(candidate.filename().string(), false) != files::Kind::Rom)
      return fs::weakly_canonical(candidate, errorCode).string();
  return {};
}

void App::projectSettings() {
  if (!project_) return;
  if (projectUnsaved_ && !createProjectFile()) return;  // the settings are the project file
  if (OpenFile* file = openFile(project_->file); file && file->form && !file->dirty()) file->form->showText(file->editor, false);
}

void App::projectFileSaved() {
  Project loadedProject;
  std::string error;
  if (!loadedProject.load(project_->file, &error)) {
    output_ += error + "\n";
    return;
  }
  project_ = loadedProject;
  SDL_SetWindowTitle(window_, ("VCSIDE - " + loadedProject.name).c_str());
  for (auto& file : files_) lint(*file);
}

void App::openProject(const std::string& file) {
  if (hooks.raiseIfOpen && hooks.raiseIfOpen(fs::path(file).parent_path().string(), this)) return;
  Project loadedProject;
  std::string error;
  if (!loadedProject.load(file, &error)) {
    output_ += error + "\n";
    return;
  }
  saveUserState();  // the project this window leaves
  project_ = loadedProject;
  projectUnsaved_ = false;
  folder_ = loadedProject.dir;
  symbols_.clear();
  builtFiles_.clear();
  diskSymbols_.clear();
  diagnostics_.clear();
  loadSymbols();
  loadUserState();
  addRecent(loadedProject.file);
  // the files that were open, each where its cursor was; the main program if there were none
  OpenFile* front = nullptr;
  int reopened = 0;
  for (auto& [path, cursor] : savedFiles_)
    if (OpenFile* opened = openFile(path)) {
      opened->editor.goTo(cursor.line, cursor.col);
      if (path == savedActiveFile_) front = opened;
      reopened++;
    }
  if (!reopened) openFile(loadedProject.abs(loadedProject.main));
  else if (front) openFile(front->path);
  SDL_SetWindowTitle(window_, ("VCSIDE - " + loadedProject.name).c_str());
}

void App::openFolder(const std::string& dir) {
  if (hooks.raiseIfOpen && hooks.raiseIfOpen(fs::absolute(dir).string(), this)) return;
  project_.reset();
  projectUnsaved_ = false;
  folder_ = fs::absolute(dir).string();
  addRecent(folder_);
  SDL_SetWindowTitle(window_, ("VCSIDE - " + fs::path(folder_).filename().string()).c_str());
}

OpenFile* App::openFile(const std::string& pathIn, int line) {
  std::string path = fs::weakly_canonical(pathIn).string();
  for (auto& openedFile : files_) openedFile->requestFocus = false;  // the file opened last is the one shown
  for (size_t i = 0; i < files_.size(); i++)
    if (files_[i]->path == path) {
      activeFile_ = (int)i;
      files_[i]->requestFocus = true;
      if (line > 0) files_[i]->editor.goTo(line - 1);
      return files_[i].get();
    }
  std::error_code errorCode;
  if (!fs::exists(path, errorCode)) return nullptr;
  if (lower(fs::path(path).extension().string()) == ".ttt") {  // a TIATracker song: becomes a .vcsmusic
    std::string musicPath = importTiaTrackerFile(path);
    return musicPath.empty() ? nullptr : openFile(musicPath);
  }
  auto newFile = std::make_unique<OpenFile>();
  newFile->path = path;
  if (ImageViewer::handles(path)) {
    newFile->asmSyntax = false;
    newFile->image = std::make_unique<ImageViewer>();
    std::string error;
    if (!newFile->image->load(path, &error)) {
      output_ += F("Cannot read {0}: {1}", {path, error}) + "\n";
      return nullptr;
    }
    newFile->requestFocus = true;
    files_.push_back(std::move(newFile));
    activeFile_ = (int)files_.size() - 1;
    return files_.back().get();
  }
  if (lower(fs::path(path).extension().string()) == ".vcsproj") {
    newFile->asmSyntax = false;
    newFile->form = std::make_unique<ProjectForm>();
    std::string error;
    if (!newFile->form->load(path, newFile->editor, &error)) {
      output_ += error + "\n";
      return nullptr;
    }
    newFile->requestFocus = true;
    files_.push_back(std::move(newFile));
    activeFile_ = (int)files_.size() - 1;
    return files_.back().get();
  }
  if (lower(fs::path(path).extension().string()) == ".vcsmusic") {
    newFile->asmSyntax = false;
    newFile->music = std::make_unique<music::MusicEditor>();
    std::string error;
    if (!newFile->music->load(path, &error)) {
      output_ += F("Cannot read {0}: {1}", {path, error}) + "\n";
      return nullptr;
    }
    newFile->requestFocus = true;
    files_.push_back(std::move(newFile));
    activeFile_ = (int)files_.size() - 1;
    return files_.back().get();
  }
  if (lower(fs::path(path).extension().string()) == ".vcsgfx") {
    newFile->asmSyntax = false;
    newFile->gfx = std::make_unique<GfxEditor>();
    std::string error;
    if (!newFile->gfx->load(path, &error)) {
      output_ += F("Cannot read {0}: {1}", {path, error}) + "\n";
      return nullptr;
    }
    newFile->requestFocus = true;
    files_.push_back(std::move(newFile));
    activeFile_ = (int)files_.size() - 1;
    return files_.back().get();
  }
  newFile->asmSyntax = isAsmFile(path);
  newFile->editor.setText(readFile(path));
  newFile->editor.setGutterWidthChars(gutterChars(*newFile));
  std::string ext = lower(fs::path(path).extension().string());
  bool cLike = ext == ".c" || ext == ".cpp" || ext == ".hpp" || (ext == ".h" && project_ && isC(project_->language));
  if (cLike) newFile->asmSyntax = false;
  newFile->editor.setCommentPrefix(cLike ? "//" : ext == ".bas" ? "rem" : ";");
  newFile->editor.setFoldSyntax(cLike ? FoldSyntax::C : ext == ".bas" ? FoldSyntax::Basic : newFile->asmSyntax ? FoldSyntax::Asm : FoldSyntax::None);
  newFile->editor.setDefaultIndent(cLike ? IndentStyle{false, 4} : IndentStyle{true, 4});  // as the templates
  {
    EditorHooks editorHooks = newFile->asmSyntax ? asmHooks() : ext == ".bas" ? basicHooks() : EditorHooks{};
    OpenFile* self = newFile.get();
    // the files a line names (include, incbin, #include...)
    editorHooks.hasFile = [this, self](const std::string& name) { return !linkedFile(name, self->path).empty(); };
    editorHooks.openFile = [this, self](const std::string& name) {
      std::string linkedPath = linkedFile(name, self->path);
      if (!linkedPath.empty()) openFile(linkedPath);
    };
    if (newFile->asmSyntax || ext == ".bas") {
    editorHooks.gotoDefinition = [this, self](const std::string& word) { return gotoDefinition(word, self->path, self->editor.cursor().line); };
    editorHooks.hasDefinition = [this, self](const std::string& word, int line) {
      std::string path;
      int foundLine = 0;
      return findDefinition(word, self->path, line, path, foundLine);
    };
    }
    if (newFile->asmSyntax) {
      // the cycle column: cycles, or (a click switches every editor) where each line's bytes are
      editorHooks.gutter = [this, self, cycles = editorHooks.gutter](int line, const std::string& text) {
        return columnAddresses_ ? addressColumn(self->path, line) : cycles(line, text);
      };
      editorHooks.lineInfo = [this, self](int line) { return lineDetails(*self, line); };
      editorHooks.gutterClick = [this] { switchCycleColumn(); };
    }
    newFile->editor.setHooks(editorHooks);
  }
  updatePageSeparators(*newFile);
  newFile->requestFocus = true;
  if (line > 0) newFile->editor.goTo(line - 1);
  if (auto found = breakpoints_.find(path); found != breakpoints_.end()) newFile->editor.setBreakpoints(found->second);
  lint(*newFile);
  files_.push_back(std::move(newFile));
  activeFile_ = (int)files_.size() - 1;
  return files_.back().get();
}

gfx::Profile App::gfxProfile() {
  if (!project_ || project_->language != Language::BatariBasic) {
    gfx::Profile profile;
    profile.usesC = project_ && isC(project_->language);
    return profile;
  }
  std::string main = project_->abs(project_->main);
  std::error_code errorCode;
  auto writeTime = fs::last_write_time(main, errorCode);
  if (main != gfxProfileFile_ || writeTime != gfxProfileTime_) {
    gfxProfileFile_ = main;
    gfxProfileTime_ = writeTime;
    gfxProfile_ = gfx::profileFromBasic(errorCode ? std::string() : readFile(main));
  }
  return gfxProfile_;
}
gfx::Tv App::gfxTv() const { return gfx::tvFromProject(project_ ? project_->tv : "NTSC"); }

bool App::saveFile(OpenFile& file) {
  if (file.image) return true;  // nothing to save: the viewer never changes it
  if (file.form) {
    std::string text, error;
    if (!file.form->contents(file.editor, &text, &error)) {
      output_ += F("Not saved: {0}", {error}) + "\n";
      lastStatus_ = F("Not saved: {0}", {error});
      return false;
    }
    std::ofstream out(file.path, std::ios::binary);
    if (!out) {
      output_ += F("Cannot write {0}", {file.path}) + "\n";
      return false;
    }
    out << text;
    out.close();
    file.form->saved(file.editor, text);
    if (project_ && fs::weakly_canonical(project_->file) == fs::weakly_canonical(file.path)) projectFileSaved();
    return true;
  }
  if (file.music) {
    std::string error;
    if (!file.music->save(file.path, &error)) {
      output_ += error + "\n";
      return false;
    }
    lastStatus_ = F("Saved {0}", {fs::path(file.path).filename().string()});
    return true;
  }
  if (file.gfx) {
    std::string error, generated;
    if (!file.gfx->save(file.path, gfxProfile(), &error, &generated)) {
      output_ += error + "\n";
      return false;
    }
    if (!generated.empty()) {
      lastStatus_ = F("Saved; the tables are in {0}", {fs::path(generated).filename().string()});
      // an open copy of the generated file shows the new tables
      std::string canon = fs::weakly_canonical(generated).string();
      for (auto& openedFile : files_)
        if (openedFile->path == canon && !openedFile->gfx && !openedFile->editor.dirty()) openedFile->editor.setText(readFile(canon)), openedFile->editor.markSaved();
    }
    updateBasicRegions(file);
    return true;
  }
  std::ofstream out(file.path, std::ios::binary);
  if (!out) {
    output_ += F("Cannot write {0}", {file.path}) + "\n";
    return false;
  }
  out << file.editor.text() << "\n";
  file.editor.markSaved();
  return true;
}

// The batari Basic programs of the project that hold regions of this graphics file
// (`rem vcsgfx <file> <item>` ... `rem vcsgfx end`) get the new blocks: in the editor if
// the program is open (saved too, unless it had changes of its own), on disk if not.
void App::updateBasicRegions(OpenFile& graphicsFile) {
  std::string name = fs::path(graphicsFile.path).filename().string();
  gfx::Profile profile = graphicsFile.gfx->profile(gfxProfile());
  fs::path root = project_ ? fs::path(project_->dir) : fs::path(graphicsFile.path).parent_path();
  std::error_code errorCode;
  std::vector<std::string> programs;
  for (auto iterator = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, errorCode);
       !errorCode && iterator != fs::recursive_directory_iterator(); iterator.increment(errorCode)) {
    std::string fileName = iterator->path().filename().string();
    if (iterator->is_directory(errorCode) && (fileName.empty() || fileName[0] == '.' || fileName == "build" || iterator.depth() >= 3)) {
      iterator.disable_recursion_pending();
      continue;
    }
    if (lower(iterator->path().extension().string()) == ".bas") programs.push_back(fs::weakly_canonical(iterator->path(), errorCode).string());
  }
  std::vector<std::string> touched;
  for (auto& path : programs) {
    OpenFile* open = nullptr;
    for (auto& openedFile : files_)
      if (openedFile->path == path && !openedFile->gfx) open = openedFile.get();
    std::string text = open ? open->editor.text() : readFile(path);
    if (text.find("vcsgfx") == std::string::npos) continue;
    int regionCount = 0;
    std::vector<std::string> problems;
    std::string updated = gfx::updateBasicRegions(text, name, graphicsFile.gfx->asset(), profile, &regionCount, &problems);
    for (auto& problem : problems) output_ += fs::path(path).filename().string() + ": " + problem + "\n";
    if (updated == text) continue;
    touched.push_back(fs::path(path).filename().string());
    if (open) {
      bool hadChanges = open->editor.dirty();
      open->editor.replaceText(updated);
      if (!hadChanges) saveFile(*open);
    } else {
      std::ofstream out(path, std::ios::binary);
      if (out) out << updated;
      else output_ += F("Cannot write {0}", {path}) + "\n";
    }
  }
  if (!touched.empty()) {
    std::string list;
    for (auto& touchedName : touched) list += (list.empty() ? "" : ", ") + touchedName;
    lastStatus_ = F("Saved; graphics updated in {0}", {list});
  }
}

void App::saveAll() {
  for (auto& file : files_)
    if (file->dirty()) saveFile(*file);
}

// ---------------------------------------------------------------------------
// Build and run

void App::build(int thenRun) {
  if (building_) return;
  if (!project_) {
    Project candidate;
    if (!folder_.empty() && Project::detect(folder_, &candidate)) {
      buildCandidate_ = candidate, candidateRun_ = thenRun, showNoProject_ = true;
      return;
    }
    std::string why = folder_.empty() ? T("No project open.")
                                      : T("Nothing to build in this folder: it has no project file, Makefile or build script, and no DASM source (with a \"processor\" line) or batari Basic one.");
    output_ += why + "\n";
    lastStatus_ = why;
    outputScroll_ = true;
    return;
  }
  saveAll();
  for (auto& id : project_->buildTools(tools_))
    if (Tool* tool = tools_.get(id); tool && !tool->found()) {
      askForTool(id, T("build this project"), [this, thenRun] { build(thenRun); });
      return;
    }
  std::string error, workDir;
  EnvVars env;
  auto args = project_->buildArgs(tools_, &error, &workDir, &env);
  if (args.empty()) {
    output_ += error + "\n";
    lastStatus_ = error;
    outputScroll_ = true;
    return;
  }
  output_.clear();
  diagnostics_.clear();
  space_.reset();
  elfSpace_.reset();
  spaceNote_.clear();
  building_ = true;
  runAfterBuild_ = thenRun;
  lastStatus_ = T("Building...");
  if (project_->language == Language::BatariBasic && project_->buildCommand.empty()) {
    // its script ends with 0 even when it fails: whatever is left from the last build must go
    std::error_code errorCode;
    for (auto suffix : {".bin", ".asm", ".list.txt", ".symbol.txt", ".lst", ".sym"})
      fs::remove(project_->basicOutput(suffix), errorCode);
  }
  job_.start(args, workDir, env);
}

void App::pollBuild() {
  if (!building_) return;
  output_ = job_.output();
  if (job_.running()) return;
  building_ = false;
  output_ = job_.output();
  bool gcc = project_ && (isC(project_->language) || !project_->buildCommand.empty());
  bool basic = project_ && project_->language == Language::BatariBasic && project_->buildCommand.empty();
  // the first line repeats the command ("> make ..."): only what it printed is parsed
  std::string printed = output_.rfind("> ", 0) == 0 && output_.find('\n') != std::string::npos ? output_.substr(output_.find('\n') + 1) : output_;
  if (basic) diagnostics_ = parseBasicOutput(printed, project_->main, readFile(project_->basicOutput(".asm")));
  else diagnostics_ = gcc ? parseGccOutput(printed) : parseDasmOutput(printed);
  if (gcc) {
    auto dasmDiagnostics = parseDasmOutput(printed);
    diagnostics_.insert(diagnostics_.end(), dasmDiagnostics.begin(), dasmDiagnostics.end());
  }
  bool ok = job_.exitCode() == 0;
  // batari Basic's script ends with 0 even when it fails: the ROM and no errors tell
  if (basic) ok = ok && finishBasicBuild();
  std::string message = F(ok ? "Build succeeded (exit {0})" : "Build failed (exit {0})", {std::to_string(job_.exitCode())});
  output_ += "\n" + message + "\n";
  lastStatus_ = message;
  outputScroll_ = true;
  if (ok) {
    std::error_code errorCode;
    auto size = fs::file_size(project_->outputPath(), errorCode);
    if (!errorCode) output_ += F("{0}: {1} bytes", {project_->outputPath(), std::to_string(size)}) + "\n";
    loadSymbols();
    if (plusCart_.running()) updatePlusCartRoms();
    if (isC(project_->language) && (project_->language == Language::AceC || project_->format == "ACE")) measureAce();
    else if (project_->language == Language::ElfC) measureElf();
    else if (basic) measureBasic(errorCode ? 0 : (long)size);
    else startSpace();
  }
  for (auto& file : files_) lint(*file);
  if (ok && runAfterBuild_) run(runAfterBuild_ == 2);
  runAfterBuild_ = 0;
}

// batari Basic leaves <main>.bas.bin, .list.txt and .symbol.txt next to the source: the
// ROM goes where the project says and the listing and symbols get the names Stella and
// VCSIDE look for (<rom stem>.lst and .sym).
bool App::finishBasicBuild() {
  std::error_code errorCode;
  std::string romPath = project_->basicOutput(".bin");
  bool errors = std::any_of(diagnostics_.begin(), diagnostics_.end(), [](auto& diagnostic) { return diagnostic.severity == Diagnostic::Error; });
  if (errors || !fs::exists(romPath, errorCode)) {
    if (!errors) diagnostics_.push_back({Diagnostic::Error, project_->main, 0, T("batari Basic did not write the ROM (see Output).")});
    return false;
  }
  auto copy = [&](const std::string& from, const std::string& to) {
    if (fs::equivalent(from, to, errorCode)) return true;
    fs::create_directories(fs::path(to).parent_path(), errorCode);
    return fs::copy_file(from, to, fs::copy_options::overwrite_existing, errorCode);
  };
  if (!copy(romPath, project_->outputPath())) {
    output_ += F("Cannot copy {0} to {1}: {2}", {romPath, project_->outputPath(), errorCode.message()}) + "\n";
    return false;
  }
  copy(project_->basicOutput(".list.txt"), project_->stem() + ".lst");
  copy(project_->basicOutput(".symbol.txt"), project_->stem() + ".sym");
  return true;
}

void App::measureBasic(long romSize) {
  SpaceOptions options;
  options.warnPercent = project_->spaceWarnPercent;
  space_ = parseBasicSpace(output_, romSize, options);
  if (space_->banks.empty()) {
    space_.reset();
    spaceNote_ = T("batari Basic did not say how much ROM is left.");
    return;
  }
  for (auto diagnostic : space_->diagnostics) {
    diagnostic.message = T("Space") + std::string(": ") + diagnostic.message;
    diagnostics_.push_back(diagnostic);
  }
  output_ += space_->summary() + "\n";
}

// Space report: DASM again with -f2 (exact blocks of emitted bytes), then the listing for RAM.
void App::startSpace() {
  auto args = project_->rasArgs(tools_);
  if (args.empty()) {
    spaceNote_ = T("The space report is only available for DASM projects built by VCSIDE.");
    return;
  }
  spaceRunning_ = spaceJob_.start(args, project_->dir);
}

// ELF: the ROM's sections and, if it is up to date, the Chameleon image linked with the firmware.
void App::measureElf() {
  std::string rom = project_->outputPath(), chameleonPath = project_->stem() + "-chameleon.elf";
  ElfFile romElf, chameleonElf;
  std::string error;
  if (!readElf(readFile(rom), romElf, &error) || romElf.type != 1) {
    spaceNote_ = F("Cannot measure {0}: {1}", {rom, error.empty() ? T("it is not a relocatable ELF object") : error});
    return;
  }
  std::error_code errorCode;
  ElfLimits limits = elfLimits(project_->elfCartridge, project_->elfFlashKb, project_->elfRamKb);
  bool useChameleon = limits.cartridge == ElfCartridge::Chameleon && fs::exists(chameleonPath, errorCode) &&
                      fs::last_write_time(chameleonPath, errorCode) >= fs::last_write_time(rom, errorCode) &&
                      readElf(readFile(chameleonPath), chameleonElf, &error) && chameleonElf.type == 2;
  elfSpace_ = analyzeElfSpace(romElf, (long)fs::file_size(rom, errorCode), useChameleon ? &chameleonElf : nullptr, limits, project_->spaceWarnPercent);
  for (auto diagnostic : elfSpace_->diagnostics) {
    diagnostic.message = T("Space") + std::string(": ") + diagnostic.message;
    diagnostics_.push_back(diagnostic);
  }
  output_ += elfSpace_->summary() + "\n";
}

// ACE: the debug ELF the Makefile writes next to the file (build/ACE_debugging.elf), when
// it is not older than the file.
void App::measureAce() {
  std::string file = project_->outputPath();
  fs::path debugPath = fs::path(file).parent_path() / "ACE_debugging.elf";
  std::error_code errorCode;
  ElfFile debugElf;
  std::string error;
  if (!fs::exists(debugPath, errorCode) || fs::last_write_time(debugPath, errorCode) < fs::last_write_time(file, errorCode) ||
      !readElf(readFile(debugPath.string()), debugElf, &error) || debugElf.type != 2) {
    spaceNote_ = F("Cannot measure {0}: the space comes from {1}, which the Makefile writes with it.", {file, debugPath.string()});
    return;
  }
  elfSpace_ = analyzeAceSpace(debugElf, (long)fs::file_size(file, errorCode), project_->spaceWarnPercent);
  for (auto diagnostic : elfSpace_->diagnostics) {
    diagnostic.message = T("Space") + std::string(": ") + diagnostic.message;
    diagnostics_.push_back(diagnostic);
  }
  output_ += elfSpace_->summary() + "\n";
}

// The project's ROM and, for an ELF project built with make ace, its ACE file too.
void App::updatePlusCartRoms() {
  if (!project_) return;
  std::vector<ServedRom> roms;
  std::error_code errorCode;
  fs::path output = project_->outputPath();
  for (fs::path candidate : {output, fs::path(output).replace_extension(".ace")})
    if (fs::is_regular_file(candidate, errorCode) && (roms.empty() || candidate != output))
      roms.push_back({candidate.filename().string(), candidate.string()});
  plusCart_.setRoms(roms, "VCSIDE: " + project_->name);
}

void App::togglePlusCart() {
  outputScroll_ = true;
  if (plusCart_.running()) {
    plusCart_.stop();
    output_ += std::string(T("PlusCart: no longer serving.")) + "\n";
    return;
  }
  if (!project_) return;
  std::string error;
  if (!plusCart_.start(80, &error)) {
    output_ += F("PlusCart: cannot listen on port 80 ({0}). The PlusCart only asks on that port: close the program that uses it. On Linux, ports below 1024 need permission (for example, setcap cap_net_bind_service on VCSIDE).",
                 {error}) + "\n";
    return;
  }
  updatePlusCartRoms();
  std::string address = localNetworkAddress();
  if (address.empty()) address = T("this computer's address on the network");
  output_ += F("PlusCart: serving the project's ROM at {0}. On the PlusCart (on the same network): Setup > API Host, enter {0}, and its menu lists the ROM; choose it to run it. After each build, choose it again. To use the PlusStore again, set the API Host back to pluscart.firmaplus.de.",
               {address}) + "\n";
}

void App::pollSpace() {
  if (!spaceRunning_ || spaceJob_.running()) return;
  spaceRunning_ = false;
  if (!project_ || spaceJob_.exitCode() != 0) {
    spaceNote_ = T("Could not measure the space: DASM failed writing the block file.");
    return;
  }
  std::error_code errorCode;
  long romSize = (long)fs::file_size(project_->outputPath(), errorCode);
  if (errorCode) romSize = 0;
  std::string rasPath = project_->stem() + ".ras";
  auto rom = parseDasmRas(readFile(rasPath));
  fs::remove(rasPath, errorCode);
  std::string listing = readFile(project_->stem() + ".lst");
  // Listing file names are as DASM opened them: absolute, or relative to the project or an include folder.
  auto readSource = [this](const std::string& name) {
    std::vector<std::string> dirs = {project_->dir};
    for (auto& dir : project_->includeDirs) dirs.push_back(project_->abs(dir));
    fs::path path(name);
    std::error_code existsCode;
    if (!path.is_absolute())
      for (auto& dir : dirs)
        if (fs::exists(fs::path(dir) / name, existsCode)) {
          path = fs::path(dir) / name;
          break;
        }
    std::vector<std::string> lines;
    std::istringstream stream(readFile(path.string()));
    for (std::string line; std::getline(stream, line);) lines.push_back(line);
    return lines;
  };
  auto ram = parseDasmListingRam(listing, readSource, [this](const std::string& name) { return lookupSymbol(name); });
  auto code = parseDasmListingCode(listing);
  FormatSpace formatSpace;
  if (const CartFormat* cartFormat = kb_.format(project_->format)) formatSpace = cartFormat->space;
  SpaceOptions options;
  options.warnPercent = project_->spaceWarnPercent;
  options.stackReserve = project_->stackReserve;
  space_ = analyzeSpace(rom, romSize, ram, code, formatSpace, options);
  if (!formatSpace.known) spaceNote_ = F("The {0} format has no space data yet: 4 KB banks assumed.", {project_->format});
  for (auto diagnostic : space_->diagnostics) {
    diagnostic.message = T("Space") + std::string(": ") + diagnostic.message;
    diagnostics_.push_back(diagnostic);
  }
  output_ += space_->summary() + "\n";
  outputScroll_ = true;
  for (auto& file : files_) lint(*file);
}

void App::run(bool debug) {
  if (!project_) return;
  if (Tool* tool = tools_.get(project_->emulator); tool && !tool->found()) {
    askForTool(tool->id, T("run the game"), [this, debug] { run(debug); });
    return;
  }
  std::string error;
  auto args = project_->runArgs(tools_, debug, &error);
  if (debug && !args.empty()) {
    auto extra = debugArgs(project_->emulator);
    args.insert(args.end() - 1, extra.begin(), extra.end());  // before the ROM
  }
  launchEmulator(*project_, args, error);
}

// Runs a ROM chosen in the project tree with the project's emulator and options; its format
// is the one its header names (ACE), the project's for the project's own ROM, else AUTO.
void App::runRom(const std::string& rom) {
  if (!project_) return;
  Project romProject = *project_;
  std::error_code errorCode;
  bool ownRom = fs::weakly_canonical(rom, errorCode) == fs::weakly_canonical(project_->outputPath(), errorCode);
  romProject.output = rom;
  std::string detected = romFormat(rom);
  romProject.format = !detected.empty() ? detected : ownRom ? project_->format : "AUTO";
  if (!formatSupported(romProject.emulator, romProject.format)) {
    Tool* tool = tools_.get(romProject.emulator);
    output_ += F("{0} cannot run {1} cartridges: it runs in Gopher2600.", {tool ? tool->name : romProject.emulator, romProject.format}) + "\n";
    romProject.emulator = "gopher2600";
  }
  if (Tool* tool = tools_.get(romProject.emulator); tool && !tool->found()) {
    askForTool(tool->id, T("run the game"), [this, rom] { runRom(rom); });
    return;
  }
  std::string error;
  auto args = romProject.runArgs(tools_, false, &error);
  launchEmulator(romProject, args, error);
}

void App::launchEmulator(const Project& project, std::vector<std::string> args, std::string error) {
  EmulatorLaunch launch;
  launch.args = args;
  if (!args.empty() && !prepareEmulatorLaunch(project, kb_, (fs::path(configDir()) / "emulators").string(), launch, &error))
    args.clear();
  for (auto& note : launch.notes) output_ += note + "\n";
  if (args.empty() || !launchDetached(launch.args, launch.workDir, &error, launch.env)) {
    output_ += error + "\n";
    lastStatus_ = error;
  } else {
    std::string envText;
    for (auto& [key, value] : launch.env) envText += key + "=" + quoteArgs({value}) + " ";
    output_ += "> " + envText + quoteArgs(launch.args) + "\n";
  }
  outputScroll_ = true;
}

void App::loadSymbols() {
  if (!project_) return;
  std::string symbolFile = project_->stem() + ".sym";
  std::error_code errorCode;
  if (fs::exists(symbolFile, errorCode)) symbols_ = parseDasmSymbols(readFile(symbolFile));
  builtFiles_.clear();
  liveReady_ = false;  // the next scan includes these sources
  if (fs::exists(project_->stem() + ".lst", errorCode)) {
    std::istringstream in(readFile(project_->stem() + ".lst"));
    std::set<std::string> seen;
    for (std::string line; std::getline(in, line);) {
      if (line.rfind("------- FILE ", 0) != 0) continue;
      std::string name = line.substr(13, line.find(" LEVEL ") == std::string::npos ? std::string::npos : line.find(" LEVEL ") - 13);
      std::string path = resolveSource(name);
      if (!fs::is_regular_file(path, errorCode)) {  // a source that cannot be read: hide nothing
        builtFiles_.clear();
        break;
      }
      if (seen.insert(path).second) builtFiles_.push_back(path);
    }
  }
  addresses_ = AddressMap();
  addressColumns_.clear();
  std::string listingPath = project_->stem() + ".lst";
  bool basic = project_->language == Language::BatariBasic;
  if ((project_->language == Language::Dasm || basic) && fs::exists(listingPath, errorCode)) {
    long bank = 4096;
    if (const CartFormat* cartFormat = kb_.format(project_->format); cartFormat && cartFormat->space.known) bank = cartFormat->space.bankSize;
    addresses_.load(readFile(listingPath), [this](const std::string& name) { return lookupSymbol(name); }, bank);
    // the listing is of the generated .asm: its ";;line N;;" marks give the lines of the .bas
    if (basic)
      addresses_.remap(fs::path(project_->main).filename().string() + ".asm",
                       basicLineMap(readFile(project_->basicOutput(".asm"))), project_->abs(project_->main));
    addresses_.resolveFiles([this](const std::string& name) { return resolveSource(name); });
  }
  for (auto& file : files_) updatePageSeparators(*file);
}

static std::string hexAddress(long address) {
  char text[16];
  snprintf(text, sizeof text, "$%04lX", address);
  return text;
}

void App::switchCycleColumn() {
  columnAddresses_ = !columnAddresses_;
  for (auto& file : files_) file->editor.setGutterWidthChars(gutterChars(*file));
  saveSettings();
}

int App::gutterChars(const OpenFile& file) const {
  if (!showCycles_ || !file.asmSyntax) return 0;
  return 9;  // "$F6F0 256": the same in both modes, so a switch moves nothing
}

std::string App::addressColumn(const std::string& path, int line) {
  if (addresses_.empty()) return {};
  auto file = addressColumns_.find(path);
  if (file == addressColumns_.end()) {
    file = addressColumns_.emplace(path, std::map<int, const ListedLine*>{}).first;
    for (const ListedLine* listed : addresses_.linesIn(path))
      if (listed->bytes > 0) file->second.emplace(listed->line - 1, listed);
  }
  auto found = file->second.find(line);
  if (found == file->second.end()) return {};
  return hexAddress(found->second->cpu) + " " + std::to_string(found->second->bytes);
}

std::string App::lineDetails(const OpenFile& file, int line) const {
  const auto& lines = file.editor.lines();
  if (line < 0 || line >= (int)lines.size()) return {};
  LineCycles cycles = countLine(kb_, lines[line], [this](const std::string& name) { return lookupSymbol(name); });
  if (!cycles.isCode && addresses_.empty()) return {};
  std::string details;
  if (cycles.isCode) {
    if (cycles.known) {
      std::string count = std::to_string(cycles.min) + (cycles.max != cycles.min ? "-" + std::to_string(cycles.max) : "");
      details = F("{0} cycles, {1}", {count, cycles.mode});
      if (cycles.guessed) details += " " + Ts("(zero page/absolute guessed: unknown symbol, build to resolve)");
    } else {
      details = T("Cycles unknown");
    }
    details += "\n";
  }
  std::string where = addressInfo(file.path, line);
  if (!cycles.isCode && where == T("No bytes for this line in the last build.")) return {};  // a comment, a label...
  return details + where;
}

std::string App::addressInfo(const std::string& path, int line) const {
  if (addresses_.empty()) return T("No addresses yet: build first.");
  std::string info;
  int shown = 0;
  for (const ListedLine* listed : addresses_.linesIn(path)) {
    if (listed->line != line + 1) continue;
    if (++shown > 8) {
      info += "...\n";
      break;
    }
    long last = listed->cpu + listed->bytes - 1;
    std::string where = hexAddress(listed->cpu);
    if (listed->bytes > 1) where += "-" + hexAddress(last);
    if (addresses_.banks() > 1) where += "  " + F("(bank {0})", {std::to_string(addresses_.bankOf(*listed))});
    info += where + "\n";
    info += "  " + F("{0} bytes, at {1} in the ROM file", {std::to_string(listed->bytes), hexAddress(addresses_.romOffset(*listed))}) + "\n";
    if ((last >> 8) != (listed->cpu >> 8)) info += "  " + F("Crosses into page {0}.", {hexAddress(last & ~0xFFL)}) + "\n";
  }
  if (!shown) return T("No bytes for this line in the last build.");
  info.pop_back();
  return info;
}

void App::updatePageSeparators(OpenFile& file) {
  std::multimap<int, TextEditor::Separator> separators;
  if (!showPages_ || !file.asmSyntax || file.gfx || file.form || addresses_.empty()) {
    file.editor.setSeparators(separators);
    return;
  }
  for (const PageBreak& pageBreak : pageBreaks(addresses_, file.path, file.editor.lines())) {
    std::string name = pageBreak.newBank ? F("bank {0}", {std::to_string(pageBreak.bank)}) + "  " + F("page {0}", {hexAddress(pageBreak.address)})
                                         : F("page {0}", {hexAddress(pageBreak.address)});
    if (!pageBreak.newBank && addresses_.banks() > 1) name += "  " + F("(bank {0})", {std::to_string(pageBreak.bank)});
    if (pageBreak.inside) {  // the line that crosses into the page is framed, and the label says so
      name = pageBreak.newBank ? F("bank {0}", {std::to_string(pageBreak.bank)}) + "  " : "";
      name += F("page {0} starts inside this line", {hexAddress(pageBreak.address)});
      if (!pageBreak.newBank && addresses_.banks() > 1) name += "  " + F("(bank {0})", {std::to_string(pageBreak.bank)});
      separators.emplace(pageBreak.line - 1, TextEditor::Separator{name, pageBreak.newBank, true});
    } else {
      separators.emplace(pageBreak.line, TextEditor::Separator{name, pageBreak.newBank, false});
    }
  }
  file.editor.setSeparators(separators);
}

std::string App::resolveSource(const std::string& name) const {
  fs::path resolved(name);
  std::error_code errorCode;
  if (!resolved.is_absolute() && project_) {
    std::vector<std::string> dirs = {project_->dir};
    for (auto& dir : project_->includeDirs) dirs.push_back(project_->abs(dir));
    for (auto& dir : dirs)
      if (fs::exists(fs::path(dir) / name, errorCode)) {
        resolved = fs::path(dir) / name;
        break;
      }
  }
  return fs::weakly_canonical(resolved, errorCode).string();
}

// ---------------------------------------------------------------------------
// Breakpoints and debugging

void App::loadUserState() {
  breakpoints_.clear();
  if (!project_) return;
  json state;
  try {
    std::ifstream stream(project_->file + ".user");
    if (stream) state = json::parse(stream);
  } catch (...) {
  }
  savedFiles_.clear();
  savedActiveFile_.clear();
  if (!state.is_object()) return;
  std::error_code pathError;
  if (state.contains("open_files") && state["open_files"].is_array())
    for (auto& entry : state["open_files"]) {
      if (!entry.is_object() || !entry.contains("file") || !entry["file"].is_string()) continue;
      std::string path = fs::weakly_canonical(fs::path(project_->dir) / entry["file"].get<std::string>(), pathError).string();
      Coord cursor{std::max(0, entry.value("line", 1) - 1), std::max(0, entry.value("column", 1) - 1)};
      savedFiles_.push_back({path, cursor});
    }
  if (state.contains("active_file") && state["active_file"].is_string())
    savedActiveFile_ = fs::weakly_canonical(fs::path(project_->dir) / state["active_file"].get<std::string>(), pathError).string();
  if (!state.contains("breakpoints") || !state["breakpoints"].is_object()) return;
  for (auto& [file, lines] : state["breakpoints"].items()) {
    std::set<int> breakpointLines;
    for (auto& number : lines)
      if (number.is_number_integer() && number.get<int>() > 0) breakpointLines.insert(number.get<int>() - 1);
    std::error_code errorCode;
    if (!breakpointLines.empty()) breakpoints_[fs::weakly_canonical(fs::path(project_->dir) / file, errorCode).string()] = breakpointLines;
  }
}

void App::saveUserState() {
  if (!project_ || projectUnsaved_) return;
  auto projectRelative = [this](const std::string& file) {
    std::error_code errorCode;
    auto relative = fs::relative(file, project_->dir, errorCode);
    return errorCode || relative.empty() ? file : relative.generic_string();
  };
  for (auto& openedFile : files_)  // the open files' breakpoints as they are now
    if (!openedFile->editor.breakpoints().empty() || breakpoints_.count(openedFile->path)) breakpoints_[openedFile->path] = openedFile->editor.breakpoints();
  json breakpointsJson = json::object();
  for (auto& [file, lines] : breakpoints_) {
    if (lines.empty()) continue;
    json array = json::array();
    for (int line : lines) array.push_back(line + 1);
    breakpointsJson[projectRelative(file)] = array;
  }
  json openFiles = json::array();
  for (auto& openedFile : files_) {
    Coord cursor = openedFile->editor.cursor();
    openFiles.push_back({{"file", projectRelative(openedFile->path)}, {"line", cursor.line + 1}, {"column", cursor.col + 1}});
  }
  json state = {{"breakpoints", breakpointsJson}, {"open_files", openFiles}};
  if (activeFile_ >= 0 && activeFile_ < (int)files_.size()) state["active_file"] = projectRelative(files_[activeFile_]->path);
  std::ofstream(project_->file + ".user") << state.dump(2) << "\n";
}

std::string App::sessionPath() const {
  if (project_ && !projectUnsaved_) return project_->file;
  return folder_;
}

// Sends the editor's breakpoints to the emulator: Gopher2600 reads a script given
// with -script, Stella the one named after the ROM in its user folder.
std::vector<std::string> App::debugArgs(const std::string& emulator) {
  for (auto& openedFile : files_)
    if (!openedFile->editor.breakpoints().empty() || breakpoints_.count(openedFile->path)) breakpoints_[openedFile->path] = openedFile->editor.breakpoints();
  if (isC(project_->language)) {
    // C on the ARM: VCSIDE has no way to give the emulators a breakpoint on a line of C
    bool any = std::any_of(breakpoints_.begin(), breakpoints_.end(), [](auto& breakpoint) { return !breakpoint.second.empty(); });
    if (any && project_->language == Language::AceC)
      output_ += std::string(T("Breakpoints are not passed for ACE projects: set them in Gopher2600's debugger, which shows the C source (build/ACE_debugging.elf).")) + "\n";
    else if (any)
      output_ += std::string(T("Breakpoints are not passed for ELF projects: the emulators cannot stop on a line of C.")) + "\n";
    return {};
  }
  std::vector<Breakpoint> placed;
  for (auto& [file, lines] : breakpoints_)
    for (int lineIndex : lines) {
      auto isCode = [this](const ListedLine& listedLine) {
        // the listing keeps the label in its own column: look for a word that is an instruction
        std::istringstream words(listedLine.source);
        for (std::string word; words >> word;) {
          if (word[0] == ';') break;
          std::string mnemonic = upper(word.substr(0, word.find('.')));
          if (kb_.instruction(mnemonic)) return true;
        }
        return false;
      };
      std::string fileName = fs::path(file).filename().string();
      if (addresses_.empty()) {
        output_ += F("{0}:{1}: no listing to place the breakpoint: build first.", {fileName, std::to_string(lineIndex + 1)}) + "\n";
        continue;
      }
      auto resolved = addresses_.breakpointAt(file, lineIndex + 1, isCode);
      if (!resolved) {
        output_ += F("{0}:{1}: no code at or after this line; breakpoint ignored.", {fileName, std::to_string(lineIndex + 1)}) + "\n";
        continue;
      }
      char where[48];
      snprintf(where, sizeof where, "$%04lX", resolved->cpu);
      std::string bank = addresses_.banks() > 1 ? " " + F("(bank {0})", {std::to_string(resolved->bank)}) : "";
      if (resolved->at->line != lineIndex + 1)
        output_ += F("{0}:{1}: breakpoint at {2}{3}, the code of line {4}.", {fileName, std::to_string(lineIndex + 1), where, bank, std::to_string(resolved->at->line)}) + "\n";
      else
        output_ += F("{0}:{1}: breakpoint at {2}{3}.", {fileName, std::to_string(lineIndex + 1), where, bank}) + "\n";
      placed.push_back(*resolved);
    }
  std::vector<std::string> args;
  std::error_code errorCode;
  if (emulator == "gopher2600") {
    std::string path = project_->stem() + ".gopher2600.script";
    if (placed.empty()) {
      fs::remove(path, errorCode);
      return args;
    }
    std::ofstream out(path);
    for (auto& breakpoint : placed) {
      char line[64];
      if (addresses_.banks() > 1) snprintf(line, sizeof line, "BREAK PC $%04lX & BANK %d\n", breakpoint.cpu, breakpoint.bank);
      else snprintf(line, sizeof line, "BREAK PC $%04lX\n", breakpoint.cpu);
      out << line;
    }
    args = {"-script", path};
  } else if (emulator == "stella") {
    // Stella's debugger runs <userdir>/<rom name>.script when it opens, and
    // VCSIDE launches it with -userdir set to the build folder.
    fs::remove(project_->stem() + ".stella.script", errorCode);  // VCSIDE 0.1.0 wrote this one
    std::string path = project_->stem() + ".script";
    if (placed.empty()) {
      fs::remove(path, errorCode);
      return args;
    }
    std::ofstream out(path);
    // "break" toggles: start from none, so running the script again cannot clear them
    out << "clearBreaks\n";
    for (auto& breakpoint : placed) {
      char line[64];
      snprintf(line, sizeof line, "break $%04lX %d\n", breakpoint.cpu, breakpoint.bank);
      out << line;
    }
  }
  return args;
}


std::optional<int> App::lookupSymbol(const std::string& name) const {
  // a constant as the source says it now goes before the last build's value
  if (auto liveValue = liveValues_.find(name); liveValue != liveValues_.end()) return liveValue->second;
  auto found = symbols_.find(name);
  if (found != symbols_.end() && !builtGone(name)) return found->second;
  if (const Register* registerEntry = kb_.reg(name)) {
    int address = registerEntry->addr();
    if (address >= 0) return registerEntry->access == "R" && address < 0x30 ? address + 0x30 : address;
  }
  return std::nullopt;
}

static std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  for (std::string line; std::getline(stream, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

void App::refreshLiveSymbols(bool force) {
  if (activeFile_ < 0 || !files_[activeFile_]->asmSyntax) return;
  uint64_t now = SDL_GetTicks();
  if (!force && liveReady_ && now - liveTicks_ < 400) return;
  force |= !liveReady_;
  liveTicks_ = now;
  std::map<std::string, LiveSymbol> live;
  auto add = [&](const std::string& file, const std::vector<SourceSymbol>& symbols) {
    for (auto& symbol : symbols)
      if (symbol.name[0] != '.') live.emplace(symbol.name, LiveSymbol{symbol, file});  // the first definition wins
  };
  std::set<std::string> open;
  for (auto& openedFile : files_)
    if (openedFile->asmSyntax) {
      add(openedFile->path, scanAsmSymbols(openedFile->editor.lines()));
      open.insert(openedFile->path);
    }
  // the other sources of the folder, read again only when their date changes
  if ((!folder_.empty() || !builtFiles_.empty()) && (force || now - diskTicks_ > 3000)) {
    diskTicks_ = now;
    bool cHeaders = project_ && isC(project_->language);
    std::map<std::string, ScannedFile> seen;
    std::error_code errorCode;
    if (!folder_.empty()) {
      for (auto iterator = fs::recursive_directory_iterator(folder_, fs::directory_options::skip_permission_denied, errorCode);
           iterator != fs::recursive_directory_iterator(); iterator.increment(errorCode)) {
        if (errorCode) break;
        std::string name = iterator->path().filename().string();
        if (iterator->is_directory(errorCode) && (name == "build" || (!name.empty() && name[0] == '.'))) {
          iterator.disable_recursion_pending();
          continue;
        }
        if (!iterator->is_regular_file(errorCode) || !isAsmFile(iterator->path().string()) || iterator->file_size(errorCode) > 4 * 1024 * 1024) continue;
        if (cHeaders && lower(iterator->path().extension().string()) == ".h") continue;  // C, not assembly
        std::string path = fs::weakly_canonical(iterator->path(), errorCode).string();  // as the open files have it
        long long mtime = (long long)iterator->last_write_time(errorCode).time_since_epoch().count();
        auto previous = diskSymbols_.find(path);
        if (previous != diskSymbols_.end() && previous->second.mtime == mtime) {
          seen[path] = std::move(previous->second);
          continue;
        }
        seen[path] = {mtime, scanAsmSymbols(splitLines(readFile(path)))};
      }
    }
    for (auto& path : builtFiles_) {
      if (seen.count(path)) continue;
      long long mtime = (long long)fs::last_write_time(path, errorCode).time_since_epoch().count();
      if (errorCode) continue;
      auto previous = diskSymbols_.find(path);
      if (previous != diskSymbols_.end() && previous->second.mtime == mtime) {
        seen[path] = std::move(previous->second);
        continue;
      }
      seen[path] = {mtime, scanAsmSymbols(splitLines(readFile(path)))};
    }
    diskSymbols_ = std::move(seen);
  }
  for (auto& [path, scanned] : diskSymbols_)
    if (!open.count(path)) add(path, scanned.symbols);
  liveSymbols_ = std::move(live);
  liveReady_ = true;
  // constants that refer to other constants: a few passes
  liveValues_.clear();
  for (int pass = 0; pass < 6; pass++) {
    bool more = false;
    for (auto& [name, liveSymbol] : liveSymbols_) {
      if (liveSymbol.symbol.kind != SourceSymbol::Constant || liveValues_.count(name)) continue;
      const std::string& self = name;
      auto value = evalExpr(liveSymbol.symbol.value, [this, &self](const std::string& symbolName) -> std::optional<int> {
        if (symbolName == self) return std::nullopt;
        return lookupSymbol(symbolName);
      });
      if (value) liveValues_[name] = *value, more = true;
    }
    if (!more) break;
  }
}

// "label (main.asm:12)", "constant = $80 + 2" or "macro (kernel.h:40)"; full = for the tooltip
std::string App::liveSymbolText(const LiveSymbol& liveSymbol, bool full) const {
  std::string where = fs::path(liveSymbol.file).filename().string() + ":" + std::to_string(liveSymbol.symbol.line + 1);
  bool here = activeFile_ >= 0 && files_[activeFile_]->path == liveSymbol.file;
  if (liveSymbol.symbol.kind == SourceSymbol::Macro) return full ? F("{0}: macro ({1})", {liveSymbol.symbol.name, where}) : T("macro") + (here ? "" : " · " + where);
  if (liveSymbol.symbol.kind == SourceSymbol::Constant) {
    std::string text = full ? liveSymbol.symbol.name + " = " + liveSymbol.symbol.value : "= " + liveSymbol.symbol.value;
    if (auto value = liveValues_.find(liveSymbol.symbol.name); value != liveValues_.end() && full) {
      char buffer[40];
      snprintf(buffer, sizeof buffer, "  ($%04X, %d)", value->second & 0xFFFF, value->second);
      text += buffer;
    }
    if (full) text += "\n" + where;
    return text;
  }
  if (full) return F("{0}: label ({1}). Build to get its address.", {liveSymbol.symbol.name, where});
  return T("label") + (here ? "" : " · " + where);
}

std::string App::testComplete(const std::string& word) {
  if (activeFile_ < 0) return "no file\n";
  refreshLiveSymbols(true);
  EditorHooks editorHooks = files_[activeFile_]->asmSyntax ? asmHooks() : basicHooks();
  std::string result;
  for (auto& completion : editorHooks.complete(word, "")) result += completion.word + "\t" + completion.detail + "\n";
  return result + "hover: " + editorHooks.hover(word, "") + "\n";
}

// A file named by a tool: absolute, relative to the project or an include folder, or
// (ld gives just "main.c") the only file with that name in the project.
std::string App::findProjectFile(const std::string& name) const {
  std::error_code errorCode;
  fs::path candidate(name);
  if (candidate.is_absolute()) return fs::exists(candidate, errorCode) ? candidate.string() : "";
  if (!project_) return "";
  std::vector<std::string> dirs = {project_->dir};
  for (auto& dir : project_->includeDirs) dirs.push_back(project_->abs(dir));
  for (auto& dir : dirs)
    if (fs::exists(fs::path(dir) / candidate, errorCode)) return fs::weakly_canonical(fs::path(dir) / candidate, errorCode).string();
  if (candidate.has_parent_path()) return "";
  for (auto iterator = fs::recursive_directory_iterator(project_->dir, fs::directory_options::skip_permission_denied, errorCode);
       iterator != fs::recursive_directory_iterator(); iterator.increment(errorCode)) {
    if (iterator->is_directory(errorCode) && (iterator->path().filename() == "build" || iterator->path().filename().string()[0] == '.')) {
      iterator.disable_recursion_pending();
      continue;
    }
    if (iterator->path().filename() == candidate) return iterator->path().string();
  }
  return "";
}

// Build diagnostics for this file plus the illegal-opcode policy of the project.
void App::lint(OpenFile& file) {
  std::map<int, std::pair<bool, std::string>> markers;
  for (auto& diagnostic : diagnostics_) {
    if (diagnostic.line <= 0) continue;
    fs::path path(diagnostic.file);
    if (!path.is_absolute() && project_) path = fs::path(project_->dir) / path;
    std::error_code errorCode;
    if (fs::weakly_canonical(path, errorCode).string() != file.path && fs::path(diagnostic.file).filename() != fs::path(file.path).filename()) continue;
    auto& entry = markers[diagnostic.line];
    entry.first = entry.first || diagnostic.severity == Diagnostic::Error;
    entry.second += (entry.second.empty() ? "" : "\n") + diagnostic.message;
  }
  if (file.asmSyntax) {
    IllegalOpcodes policy = project_ ? project_->illegal : IllegalOpcodes::Warn;
    const auto& lines = file.editor.lines();
    for (int i = 0; i < (int)lines.size() && policy != IllegalOpcodes::Allow; i++) {
      AsmLine asmLine = splitAsmLine(lines[i]);
      std::string mnemonic = upper(asmLine.mnemonic.substr(0, asmLine.mnemonic.find('.')));
      const Instruction* instruction = kb_.instruction(mnemonic);
      if (!instruction || !instruction->illegal) continue;
      bool stable = !instruction->opcodes.empty() && instruction->opcodes[0]->stable;
      // NOP is legal by name; its illegal forms only matter with operands.
      if (policy == IllegalOpcodes::StableOnly && stable) continue;
      bool error = policy == IllegalOpcodes::Forbid || (policy == IllegalOpcodes::StableOnly && !stable);
      auto& entry = markers[i + 1];
      entry.first = entry.first || error;
      entry.second += (entry.second.empty() ? "" : "\n") + F(stable ? "Illegal opcode: {0}" : "Unstable illegal opcode: {0}", {mnemonic});
    }
  }
  file.editor.setMarkers(std::move(markers));
}

// ---------------------------------------------------------------------------
// Assembler syntax

std::vector<Token> App::tokenizeAsm(const std::string& text) const {
  std::vector<Token> tokens;
  int length = (int)text.size(), i = 0;
  bool first = true;  // first word of the line (label if at column 0)
  bool sawMnemonic = false;
  while (i < length) {
    char character = text[i];
    if (character == ';') {
      tokens.push_back({i, length, TokenKind::Comment});
      break;
    }
    if (std::isspace((unsigned char)character)) { i++; continue; }
    int tokenStart = i;
    if (character == '"') {
      i++;
      while (i < length && text[i] != '"') i++;
      i = std::min(length, i + 1);
      tokens.push_back({tokenStart, i, TokenKind::String});
    } else if (character == '$' || character == '%' || character == '#' || std::isdigit((unsigned char)character)) {
      if (character == '#') i++;
      if (i < length && (text[i] == '<' || text[i] == '>')) i++;
      int numberStart = i;
      if (i < length && (text[i] == '$' || text[i] == '%')) i++;
      while (i < length && std::isxdigit((unsigned char)text[i])) i++;
      if (i == numberStart) {  // '#' followed by a symbol
        tokens.push_back({tokenStart, i, TokenKind::Number});
        continue;
      }
      tokens.push_back({tokenStart, i, TokenKind::Number});
    } else if (std::isalpha((unsigned char)character) || character == '_' || character == '.') {
      while (i < length && (std::isalnum((unsigned char)text[i]) || text[i] == '_' || text[i] == '.')) i++;
      std::string word = text.substr(tokenStart, i - tokenStart);
      std::string upperWord = upper(word);
      std::string base = upperWord.substr(0, upperWord.find('.', 1));
      TokenKind kind = TokenKind::Symbol;
      if (first && tokenStart == 0) kind = TokenKind::Label;
      else if (!sawMnemonic) {
        if (const Instruction* instruction = kb_.instruction(base)) {
          kind = !instruction->illegal ? TokenKind::Mnemonic
              : (!instruction->opcodes.empty() && instruction->opcodes[0]->stable) ? TokenKind::IllegalMnemonic
                                                                 : TokenKind::UnstableMnemonic;
          sawMnemonic = true;
        } else {
          for (auto& directive : dasmDirectives())
            if (upperWord == directive) { kind = TokenKind::Directive; sawMnemonic = true; break; }
        }
      } else if (kb_.reg(word)) {
        kind = TokenKind::Register;
      }
      tokens.push_back({tokenStart, i, kind});
      first = false;
      continue;
    } else {
      i++;
    }
    first = false;
  }
  return tokens;
}

EditorHooks App::asmHooks() {
  EditorHooks editorHooks;
  editorHooks.tokenize = [this](const std::string& lineText) { return tokenizeAsm(lineText); };
  editorHooks.gutter = [this](int, const std::string& lineText) -> std::string {
    LineCycles cycles = countLine(kb_, lineText, [this](const std::string& result) { return lookupSymbol(result); });
    if (!cycles.isCode) return {};
    if (!cycles.known) return "?";
    std::string result = std::to_string(cycles.min);
    if (cycles.max != cycles.min) result += "/" + std::to_string(cycles.max);
    if (cycles.guessed) result += "~";
    return result;
  };
  editorHooks.hover = [this](const std::string& word, const std::string& line) -> std::string {
    std::string upperWord = upper(word.substr(0, word.find('.', 1)));
    if (const Instruction* instruction = kb_.instruction(upperWord)) {
      std::string result = upperWord + " - " + instruction->summary;
      if (instruction->illegal) result += std::string("  ") + (instruction->opcodes.empty() || instruction->opcodes[0]->stable ? T("[ILLEGAL]") : T("[ILLEGAL, UNSTABLE]"));
      if (!instruction->flags.empty()) result += "\n" + F("Flags: {0}", {instruction->flags});
      result += "\n" + instruction->description;
      LineCycles cycles = countLine(kb_, line, [this](const std::string& name) { return lookupSymbol(name); });
      if (cycles.known && cycles.mnemonic == upperWord) {
        std::string cycleText = std::to_string(cycles.min) + (cycles.max != cycles.min ? "-" + std::to_string(cycles.max) : "");
        result += "\n\n" + F("This line: {0}, {1} bytes, {2} cycles", {cycles.mode, std::to_string(cycles.bytes), cycleText});
        if (cycles.guessed) result += " " + Ts("(zero page/absolute guessed: unknown symbol, build to resolve)");
      }
      return result;
    }
    if (const Register* registerEntry = kb_.reg(word)) return registerHelp(*registerEntry);
    refreshLiveSymbols();
    auto live = liveSymbols_.find(word);
    // a macro, or a constant that evaluates now: the source says more than the last build
    if (live != liveSymbols_.end() && (live->second.symbol.kind == SourceSymbol::Macro ||
                                       (live->second.symbol.kind == SourceSymbol::Constant && liveValues_.count(word))))
      return liveSymbolText(live->second, true);
    auto found = symbols_.find(word);
    if (found != symbols_.end() && !builtGone(word)) {
      char buffer[48];
      snprintf(buffer, sizeof buffer, "%s = $%04X (%d)", word.c_str(), found->second, found->second);
      return buffer;
    }
    if (live != liveSymbols_.end()) return liveSymbolText(live->second, true);
    if (!word.empty() && word[0] == '.' && activeFile_ >= 0)
      for (auto& symbol : scanAsmSymbols(files_[activeFile_]->editor.lines()))
        if (symbol.name == word) return liveSymbolText({symbol, files_[activeFile_]->path}, true);
    return {};
  };
  editorHooks.complete = [this](const std::string& prefix, const std::string&) {
    std::vector<Completion> completions;
    for (auto& entry : kb_.complete(prefix, 40)) {
      TokenKind kind = entry.kind == KbEntry::Mnemonic ? TokenKind::Mnemonic
                    : entry.kind == KbEntry::Register ? TokenKind::Register
                                                   : TokenKind::Directive;
      if (entry.kind == KbEntry::Mnemonic) {
        const Instruction* instruction = kb_.instruction(entry.word);
        if (instruction && instruction->illegal) {
          if (project_ && project_->illegal == IllegalOpcodes::Forbid) continue;
          bool stable = !instruction->opcodes.empty() && instruction->opcodes[0]->stable;
          if (!stable && project_ && project_->illegal == IllegalOpcodes::StableOnly) continue;
          kind = stable ? TokenKind::IllegalMnemonic : TokenKind::UnstableMnemonic;
        }
      }
      completions.push_back({entry.word, entry.summary, kind});
    }
    // the sources as they are now (labels, constants and macros not built yet), then the
    // last build's symbols; the active file's .local labels too
    std::string upperPrefix = upper(prefix);
    auto matches = [&](const std::string& name) { return name != prefix && upper(name).compare(0, upperPrefix.size(), upperPrefix) == 0; };
    refreshLiveSymbols();
    std::set<std::string> seen;
    for (auto& completion : completions) seen.insert(completion.word);
    auto push = [&](const std::string& name, const std::string& detail, TokenKind kind) {
      if (completions.size() < 60 && seen.insert(name).second) completions.push_back({name, detail, kind});
    };
    if (activeFile_ >= 0 && !prefix.empty() && prefix[0] == '.')
      for (auto& symbol : scanAsmSymbols(files_[activeFile_]->editor.lines()))
        if (symbol.name[0] == '.' && matches(symbol.name)) push(symbol.name, T("local label"), TokenKind::Label);
    for (auto& [name, liveSymbol] : liveSymbols_)
      if (matches(name))
        push(name, liveSymbolText(liveSymbol, false),
             liveSymbol.symbol.kind == SourceSymbol::Macro ? TokenKind::Directive : liveSymbol.symbol.kind == SourceSymbol::Label ? TokenKind::Label : TokenKind::Symbol);
    for (auto& [name, value] : symbols_)
      if (matches(name) && !builtGone(name)) push(name, T("symbol"), TokenKind::Symbol);
    return completions;
  };
  return editorHooks;
}

std::string App::registerHelp(const Register& reg) const {
  std::string result = reg.name + " (" + reg.chip + " " + reg.address + ", " + (reg.access == "R" ? T("read") : T("write")) + ")  " + reg.bits + "\n" + reg.summary;
  if (!reg.timing.empty()) result += "\n" + F("Timing: {0}", {reg.timing});
  for (auto& issue : reg.issues) result += "\n! " + issue;
  return result;
}

// batari Basic: its keywords and variables, the registers, and the labels of the file.
EditorHooks App::basicHooks() {
  EditorHooks editorHooks;
  editorHooks.tokenize = [this](const std::string& lineText) { return tokenizeBasic(kb_, lineText); };
  editorHooks.hover = [this](const std::string& word, const std::string& line) -> std::string {
    std::string keywordName = word;
    size_t colonPos = line.find(word);
    if (colonPos != std::string::npos && line.compare(colonPos + word.size(), 1, ":") == 0 && kb_.basic(word + ":")) keywordName += ":";
    if (const BasicKeyword* keyword = kb_.basic(keywordName)) return keyword->syntax + "\n" + keyword->summaryText();
    if (const Register* registerEntry = kb_.reg(word)) return registerHelp(*registerEntry);
    return {};
  };
  editorHooks.complete = [this](const std::string& prefix, const std::string&) {
    std::vector<Completion> completions;
    if (prefix.empty()) return completions;
    std::string lowerPrefix = lower(prefix);
    for (auto& [keywordName, keyword] : kb_.basicKeywords())
      if (keywordName.compare(0, lowerPrefix.size(), lowerPrefix) == 0 && completions.size() < 40)
        completions.push_back({keywordName, keyword.summaryText(), keyword.kind == "variable" ? TokenKind::Register
                                           : keyword.kind == "directive" || keyword.kind == "block" ? TokenKind::Directive
                                                                                         : TokenKind::Mnemonic});
    std::string upperPrefix = upper(prefix);
    for (auto& registerEntry : kb_.registers())
      if (registerEntry.name.compare(0, upperPrefix.size(), upperPrefix) == 0 && completions.size() < 50) completions.push_back({registerEntry.name, registerEntry.summary, TokenKind::Register});
    if (activeFile_ >= 0) {
      std::set<std::string> seen;
      for (auto& lineText : files_[activeFile_]->editor.lines()) {
        std::string label = basicLabelOf(lineText);
        if (!label.empty() && label != prefix && label.compare(0, prefix.size(), prefix) == 0 && seen.insert(label).second && completions.size() < 60)
          completions.push_back({label, T("label"), TokenKind::Label});
      }
    }
    return completions;
  };
  return editorHooks;
}

bool App::testMenu(const std::string& path) {
  if (nativeMenu_) return nativeMenuPerform(path);
  MenuBar bar = buildMenuBar(false);
  const std::vector<MenuItem>* level = &bar;
  size_t start = 0;
  for (;;) {
    size_t slash = path.find('/', start);
    std::string name = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    auto found = std::find_if(level->begin(), level->end(), [&](const MenuItem& item) { return !item.separator && item.label == name; });
    if (found == level->end() || !found->enabled) return false;
    if (slash == std::string::npos) {
      if (!found->action) return false;
      found->action();
      return true;
    }
    level = &found->children;
    start = slash + 1;
  }
}

// The steps of the zoom, as in most editors.
static const float kZoomSteps[] = {0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};

void App::zoomEditor(int steps) {
  float newZoom = 1.0f;
  if (steps != 0) {
    int stepCount = (int)(sizeof kZoomSteps / sizeof kZoomSteps[0]), nearest = 0;
    for (int i = 0; i < stepCount; i++)
      if (std::fabs(kZoomSteps[i] - zoom_) < std::fabs(kZoomSteps[nearest] - zoom_)) nearest = i;
    newZoom = kZoomSteps[std::clamp(nearest + steps, 0, stepCount - 1)];
  }
  if (newZoom == zoom_) return;
  zoom_ = newZoom;
  lastStatus_ = F("Zoom: {0} %", {std::to_string((int)(newZoom * 100 + 0.5f))});
  saveSettings();
}

ImFont* App::editorFont() const { return mono_; }

void App::testSelect(int firstLine, int lastLine) {
  if (activeFile_ < 0) return;
  auto& textEditor = files_[activeFile_]->editor;
  textEditor.goTo(firstLine - 1);
  textEditor.selectLines(firstLine - 1, lastLine - 1);
}

// Is `line` the definition of `word`? (label at column 0, "word = x", "word equ x", "mac word")
static bool definesWord(const std::string& line, const std::string& word) {
  AsmLine asmLine = splitAsmLine(line);
  if (!asmLine.label.empty()) return asmLine.label == word;
  if (upper(asmLine.mnemonic) == "MAC" || upper(asmLine.mnemonic) == "MACRO") {
    std::string macroName = asmLine.operand.substr(0, asmLine.operand.find_first_of(" \t,"));
    return macroName == word;
  }
  return false;
}

bool App::findDefinition(const std::string& word, const std::string& fromFile, int fromLine, std::string& path, int& line) {
  if (word.empty()) return false;
  bool isBasic = lower(fs::path(fromFile).extension().string()) == ".bas";
  auto defines = [&](const std::string& line) { return isBasic ? basicLabelOf(line) == word : definesWord(line, word); };
  auto searched = [&](const std::string& path) { return isBasic ? lower(fs::path(path).extension().string()) == ".bas" : isAsmFile(path); };
  // 1. The open file: the closest definition above the cursor (local .labels repeat
  //    per SUBROUTINE), otherwise the first one below.
  for (auto& openedFile : files_) {
    if (openedFile->path != fromFile) continue;
    const auto& lines = openedFile->editor.lines();
    int best = -1;
    for (int i = 0; i < (int)lines.size(); i++)
      if (defines(lines[i]) && (best < 0 || i <= fromLine)) best = i;
    if (best >= 0) {
      path = openedFile->path, line = best + 1;
      return true;
    }
  }
  // 2. Other source files of the project (not build output).
  if (folder_.empty()) return false;
  std::error_code errorCode;
  for (auto iterator = fs::recursive_directory_iterator(folder_, fs::directory_options::skip_permission_denied, errorCode);
       iterator != fs::recursive_directory_iterator(); iterator.increment(errorCode)) {
    if (errorCode) break;
    std::string name = iterator->path().filename().string();
    if (iterator->is_directory() && (name == "build" || (!name.empty() && name[0] == '.'))) {
      iterator.disable_recursion_pending();
      continue;
    }
    if (!iterator->is_regular_file() || !searched(iterator->path().string()) || iterator->path().string() == fromFile) continue;
    std::ifstream stream(iterator->path());
    std::string text;
    for (int lineNumber = 1; std::getline(stream, text); lineNumber++) {
      if (!text.empty() && text.back() == '\r') text.pop_back();
      if (defines(text)) {
        path = iterator->path().string(), line = lineNumber;
        return true;
      }
    }
  }
  return false;
}

bool App::gotoDefinition(const std::string& word, const std::string& fromFile, int fromLine) {
  std::string path;
  int line = 0;
  if (findDefinition(word, fromFile, fromLine, path, line)) {
    openFile(path, line);
    return true;
  }
  if (!word.empty()) lastStatus_ = F("No definition found for {0}", {word});
  return false;
}

void App::showDoc(const std::string& word) {
  docTopic_ = word;
  focusDocs_ = true;
}

// ---------------------------------------------------------------------------
// UI

void App::closeProject() {
  for (auto& file : files_)
    if (file->dirty()) {
      quitConfirm_ = true, closeProjectAfterConfirm_ = true;
      return;
    }
  doCloseProject();
}

void App::doCloseProject() {
  plusCart_.stop();
  saveUserState();
  files_.clear();
  activeFile_ = -1;
  closeRequest_ = -1;
  project_.reset();
  projectUnsaved_ = false;
  folder_.clear();
  symbols_.clear();
  builtFiles_.clear();
  diskSymbols_.clear();
  diagnostics_.clear();
  treeToggled_.clear();
  SDL_SetWindowTitle(window_, "VCSIDE");
}

void App::requestQuit() {
  closeProjectAfterConfirm_ = false;
  for (auto& file : files_)
    if (file->dirty()) {
      quitConfirm_ = true;
      return;
    }
  quit_ = true;
}

void App::frame() {
  syncSettings();
  if (plusCart_.running())
    for (auto& line : plusCart_.takeLog()) output_ += line + "\n", outputScroll_ = true;
  {
    std::lock_guard<std::mutex> lock(dialogMutex_);
    while (!dialogResults_.empty()) {
      auto [kind, path] = dialogResults_.front();
      dialogResults_.pop_front();
      if (kind == "project") openPath(path);
      else if (kind == "folder") openPath(path);
      else if (kind == "file") openFile(path);
      else if (kind == "newdir") npDir_ = path;
      else if (kind == "npcdfj") {  // New project: checked now, copied once the project exists
        npCdfjError_.clear();
        if (Project::checkCdfjDriver(path, &npCdfjError_)) npCdfjDriver_ = path;
        else npCdfjDriver_.clear();
      } else if (kind == "cdfjdriver" && project_) {
        std::string error;
        if (Project::addCdfjDriver(path, project_->dir, &error)) {
          output_ += F("{0} copied into the project as {1}.", {path, Project::kCdfjDriverFile}) + "\n";
          build(0);
        } else {
          output_ += error + "\n", lastStatus_ = error;
          diagnostics_.push_back({Diagnostic::Error, path, 0, error});
        }
        outputScroll_ = true;
      }
      else if (kind == "themeimport") {
        std::string id, error;
        if (themes_.import(path, &id, &error)) useTheme(id), themeEditor_.open = true;
        else lastStatus_ = error;
      } else if (kind.rfind("themeexport:", 0) == 0) {
        std::string exportPath = path;
        if (lower(fs::path(exportPath).extension().string()) != ".vcstheme") exportPath += ".vcstheme";
        std::ofstream out(exportPath, std::ios::binary);
        if (out << themes_.exportJson(kind.substr(12))) lastStatus_ = F("Theme saved in {0}", {exportPath});
        else lastStatus_ = F("Cannot write {0}", {exportPath});
      }
      else if (kind == "importttt") {
        openFile(path);  // a .ttt is imported as it opens
      }
      else if (kind == "newmusic") {
        std::string musicPath = path;
        if (lower(fs::path(musicPath).extension().string()) != ".vcsmusic") musicPath += ".vcsmusic";
        std::ofstream out(musicPath, std::ios::binary);
        if (out << music::toJson(music::blankSong())) out.close(), openFile(musicPath);
        else output_ += F("Cannot write {0}", {musicPath}) + "\n";
      }
      else if (kind == "newgfx") {
        std::string graphicsPath = path;
        if (lower(fs::path(graphicsPath).extension().string()) != ".vcsgfx") graphicsPath += ".vcsgfx";
        GfxEditor graphicsEditor;
        std::string error;
        graphicsEditor.asset() = GfxEditor::blank();
        if (graphicsEditor.save(graphicsPath, gfxProfile(), &error)) openFile(graphicsPath);
        else output_ += error + "\n";
      }
      else if (kind.rfind("tool:", 0) == 0) {
        if (Tool* tool = tools_.get(kind.substr(5))) tool->userPath = path;
        tools_.detect();
        saveSettings();
      }
    }
  }
  pollBuild();
  pollSpace();
  refreshLiveSymbols();  // the cycle counts use the constants as they are written now

  // global shortcuts
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsKeyPressed(ImGuiKey_F7)) build(0);
  if (ImGui::IsKeyPressed(ImGuiKey_F5)) build(1);
  if (ImGui::IsKeyPressed(ImGuiKey_F6)) build(2);
  if (ImGui::IsKeyPressed(ImGuiKey_F1) && activeFile_ >= 0) {
    auto& textEditor = files_[activeFile_]->editor;
    const std::string& lineText = textEditor.lines()[textEditor.cursor().line];
    int wordStart = textEditor.cursor().col, wordEnd = wordStart;
    while (wordStart > 0 && (std::isalnum((unsigned char)lineText[wordStart - 1]) || lineText[wordStart - 1] == '_')) wordStart--;
    while (wordEnd < (int)lineText.size() && (std::isalnum((unsigned char)lineText[wordEnd]) || lineText[wordEnd] == '_')) wordEnd++;
    if (wordEnd > wordStart) showDoc(lineText.substr(wordStart, wordEnd - wordStart));
  }
  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G, false) && !addresses_.empty()) showGoToAddress_ = true;
  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false) && hooks.newWindow) hooks.newWindow();
  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_W, false)) requestQuit();
  // zoom (Ctrl/Cmd + "+", "-", "0"): main.cpp, from the SDL key events
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
    if (io.KeyShift) saveAll();
    else if (activeFile_ >= 0) saveFile(*files_[activeFile_]);
  }

  menuBar();
  if (folder_.empty() && files_.empty()) {
    welcomeScreen();  // nothing open: only the welcome page, no panels
  } else {
    dockspace();
    projectPanel();
    editorsPanel();
    outputPanel();
    problemsPanel();
    spacePanel();
    docsPanel();
    assistantPanel();
  }
  assistant_.dialogs();
  if (assistant_.takeChanged()) saveSettings();
  newProjectDialog();
  toolsDialog();
  noProjectDialog();
  themeEditor_.draw(themes_, themeId_,
                    {[this](const std::string& id) { useTheme(id); },
                     [this] {
                       static const SDL_DialogFileFilter filters[] = {{"VCSIDE theme", "vcstheme"}};
                       SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "themeimport"}, window_, filters, 1, nullptr, false);
                     },
                     [this](const std::string& id) {
                       static const SDL_DialogFileFilter filters[] = {{"VCSIDE theme", "vcstheme"}};
                       std::string where = (fs::path(SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS) ? SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS) : ".") / (id + ".vcstheme")).string();
                       SDL_ShowSaveFileDialog(dialogCallback, new DialogCtx{this, "themeexport:" + id}, window_, filters, 1, where.c_str());
                     },
                     [](const std::string& dir) {
                       std::error_code errorCode;
                       fs::create_directories(dir, errorCode);
                       SDL_OpenURL(("file://" + fs::absolute(dir).string()).c_str());
                     }});
  aboutDialog();
  missingToolDialog();
  closeDialogs();
  fileDialogs();
  goToAddressDialog();
  emulatorOptionsDialog();
}

// The menu bar as data: ImGui draws it on Windows and Linux, macOS puts it in the
// system menu bar. Actions look up the active file when they run.
MenuBar App::buildMenuBar(bool native) {
  auto item = [](const std::string& label, const std::string& shortcut, std::function<void()> action,
                 bool enabled = true, bool checked = false) {
    MenuItem menuItem;
    menuItem.label = label, menuItem.shortcut = shortcut, menuItem.action = std::move(action), menuItem.enabled = enabled, menuItem.checked = checked;
    return menuItem;
  };
  auto sub = [](const std::string& label, std::vector<MenuItem> children, bool enabled = true) {
    MenuItem menuItem;
    menuItem.label = label, menuItem.children = std::move(children), menuItem.enabled = enabled;
    return menuItem;
  };
  auto editor = [this]() -> TextEditor* { return activeFile_ >= 0 ? &files_[activeFile_]->editor : nullptr; };
  auto onEditor = [editor](void (*action)(TextEditor&)) { return [editor, action] { if (TextEditor* textEditor = editor()) action(*textEditor); }; };
  bool hasFile = activeFile_ >= 0;
  MenuBar bar;

  // Settings: in the application menu on macOS, a menu of its own elsewhere.
  std::vector<MenuItem> langs;
  for (auto& language : languages()) {
    MenuItem languageItem = item(language.name, "", [this, code = language.code] {
      setLanguage(code);
      saveSettings();
      if (nativeMenu_) nativeMenuRefresh();
    }, true, currentLanguage() == language.code);
    if (language.total > 0 && language.translated < language.total) languageItem.hint = std::to_string(100 * language.translated / language.total) + " %";
    langs.push_back(languageItem);
  }
  std::vector<MenuItem> settings = {item(native ? T("Tools...") : T("Tools"), native ? "Ctrl+," : "", [this] { showTools_ = true; }),
                                    item(T("Assistant..."), "", [this] { assistant_.openSettings(); }),
                                    sub(T("Language"), langs)};
  if (native) bar.push_back(sub("VCSIDE", settings));

  // File
  std::vector<MenuItem> recent;
  for (auto& recentPath : recent_) recent.push_back(item(recentPath, "", [this, recentPath] { openPath(recentPath); }));
  std::vector<MenuItem> file = {
      item(T("New project..."), "", [this] { showNewProject_ = true; }),
      item(T("Open project..."), "", [this] {
        static const SDL_DialogFileFilter filters[] = {{"VCSIDE project", "vcsproj"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "project"}, window_, filters, 2, nullptr, false);
      }),
      item(T("Open folder..."), "", [this] { showFolderDialog(dialogCallback, new DialogCtx{this, "folder"}, window_, nullptr); }),
      sub(T("Recent projects"), recent, !recent.empty()),
      item(T("New window"), "Ctrl+Shift+N", [] { if (hooks.newWindow) hooks.newWindow(); }),
      item(T("New graphics..."), "", [this] {
        static const SDL_DialogFileFilter filters[] = {{"VCSIDE graphics", "vcsgfx"}};
        std::string where = ((project_ ? fs::path(project_->dir) : fs::path(folder_.empty() ? "." : folder_)) / "graphics.vcsgfx").string();
        SDL_ShowSaveFileDialog(dialogCallback, new DialogCtx{this, "newgfx"}, window_, filters, 1, where.c_str());
      }),
      item(T("New music..."), "", [this] {
        static const SDL_DialogFileFilter filters[] = {{"VCSIDE music", "vcsmusic"}};
        std::string where = ((project_ ? fs::path(project_->dir) : fs::path(folder_.empty() ? "." : folder_)) / "music.vcsmusic").string();
        SDL_ShowSaveFileDialog(dialogCallback, new DialogCtx{this, "newmusic"}, window_, filters, 1, where.c_str());
      }),
      item(T("Import TIATracker song..."), "", [this] {
        static const SDL_DialogFileFilter filters[] = {{"TIATracker song", "ttt"}};
        SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "importttt"}, window_, filters, 1, nullptr, false);
      }),
      MenuItem::sep(),
      item(T("Save"), "Ctrl+S", [this] { if (activeFile_ >= 0) saveFile(*files_[activeFile_]); }, hasFile),
      item(T("Save all"), "Ctrl+Shift+S", [this] { saveAll(); }),
      item(T("Close file"), "", [this] { if (activeFile_ >= 0) closeRequest_ = activeFile_; }, hasFile),
      item(T("Close project"), "", [this] { closeProject(); }, hasProject()),
      item(T("Close window"), "Ctrl+Shift+W", [this] { requestQuit(); })};
  if (!native) {
    file.push_back(MenuItem::sep());
    file.push_back(item(T("Quit"), "", [] { if (hooks.quitAll) hooks.quitAll(); }));
  }
  bar.push_back(sub(T("File"), file));

  // Edit
  bool asmFile = hasFile && files_[activeFile_]->asmSyntax;
#ifdef __APPLE__
  const char* replaceKey = "Ctrl+Shift+F";  // Cmd+H hides the app on macOS
#else
  const char* replaceKey = "Ctrl+H";
#endif
  bar.push_back(sub(T("Edit"), {
      item(T("Undo"), "Ctrl+Z", [this, editor] {
        if (activeFile_ >= 0 && files_[activeFile_]->gfx) files_[activeFile_]->gfx->undo();
        else if (activeFile_ >= 0 && files_[activeFile_]->music) files_[activeFile_]->music->undo();
        else if (TextEditor* textEditor = editor()) textEditor->undo();
      }),
      item(T("Redo"), "Ctrl+Shift+Z", [this, editor] {
        if (activeFile_ >= 0 && files_[activeFile_]->gfx) files_[activeFile_]->gfx->redo();
        else if (activeFile_ >= 0 && files_[activeFile_]->music) files_[activeFile_]->music->redo();
        else if (TextEditor* textEditor = editor()) textEditor->redo();
      }),
      MenuItem::sep(),
      item(T("Cut"), "Ctrl+X", onEditor([](TextEditor& textEditor) { textEditor.cut(); })),
      item(T("Copy"), "Ctrl+C", onEditor([](TextEditor& textEditor) { textEditor.copy(); })),
      item(T("Paste"), "Ctrl+V", onEditor([](TextEditor& textEditor) { textEditor.paste(); })),
      item(T("Select all"), "Ctrl+A", onEditor([](TextEditor& textEditor) { textEditor.selectAll(); })),
      MenuItem::sep(),
      item(T("Find..."), "Ctrl+F", onEditor([](TextEditor& textEditor) { textEditor.openFind(false); })),
      item(T("Replace..."), replaceKey, onEditor([](TextEditor& textEditor) { textEditor.openFind(true); })),
      item(T("Find next"), "F3", onEditor([](TextEditor& textEditor) { textEditor.findNext(false); })),
      item(T("Find previous"), "Shift+F3", onEditor([](TextEditor& textEditor) { textEditor.findNext(true); })),
      item(T("Go to line..."), "Ctrl+G", onEditor([](TextEditor& textEditor) { textEditor.openGoto(); })),
      item(T("Go to address..."), "Ctrl+Shift+G", [this] { showGoToAddress_ = true; }, !addresses_.empty()),
      item(T("Go to definition"), "F12", [this] {
        if (activeFile_ < 0) return;
        auto& openedFile = *files_[activeFile_];
        gotoDefinition(openedFile.editor.wordAtCursor(), openedFile.path, openedFile.editor.cursor().line);
      }, asmFile),
      MenuItem::sep(),
      item(T("Toggle comment"), "Ctrl+/", onEditor([](TextEditor& textEditor) { textEditor.toggleComment(); })),
      item(T("Duplicate lines"), "Ctrl+D", onEditor([](TextEditor& textEditor) { textEditor.duplicateLines(); })),
      item(T("Move lines up"), "Alt+Up", onEditor([](TextEditor& textEditor) { textEditor.moveLines(-1); })),
      item(T("Move lines down"), "Alt+Down", onEditor([](TextEditor& textEditor) { textEditor.moveLines(1); })),
      item(T("Delete lines"), "Ctrl+Shift+K", onEditor([](TextEditor& textEditor) { textEditor.deleteLines(); })),
      MenuItem::sep(),
      item(T("Fold"), "Ctrl+Alt+Up", onEditor([](TextEditor& textEditor) { textEditor.foldAt(textEditor.cursor().line); })),
      item(T("Unfold"), "Ctrl+Alt+Down", onEditor([](TextEditor& textEditor) { textEditor.unfoldAt(textEditor.cursor().line); })),
      item(T("Fold all"), "", onEditor([](TextEditor& textEditor) { textEditor.foldAll(); })),
      item(T("Unfold all"), "", onEditor([](TextEditor& textEditor) { textEditor.unfoldAll(); })),
  }, hasFile));

  // Build
  bool canBuild = (project_.has_value() || !folder_.empty()) && !building_;  // a folder: build() looks at what it has
  std::vector<MenuItem> buildMenu = {item(T("Build"), "F7", [this] { build(0); }, canBuild),
                             item(T("Build and run"), "F5", [this] { build(1); }, canBuild),
                             item(T("Build and debug"), "F6", [this] { build(2); }, canBuild),
                             item(T("Run"), "", [this] { run(false); }, canBuild),
                             item(T("Serve to the PlusCart (Wi-Fi)"), "", [this] { togglePlusCart(); }, project_.has_value() || plusCart_.running(),
                                  plusCart_.running()),
                             MenuItem::sep(),
                             item(T("Toggle breakpoint"), "F9", onEditor([](TextEditor& textEditor) { textEditor.toggleBreakpoint(textEditor.cursor().line); }), hasFile),
                             item(T("Clear all breakpoints"), "", [this] {
                               for (auto& openedFile : files_) openedFile->editor.setBreakpoints({});
                               breakpoints_.clear();
                               saveUserState();
                             })};
  if (project_) {
    auto set = [this](std::function<void(Project&)> change) {
      return [this, change] {
        if (!project_) return;
        change(*project_);
        std::string error;
        if (!projectUnsaved_) project_->save(&error);  // a detected folder keeps it until its project file is made
        // an open settings tab with no changes of its own shows the new value
        for (auto& openedFile : files_)
          if (openedFile->form && !openedFile->dirty() && fs::weakly_canonical(openedFile->path) == fs::weakly_canonical(project_->file)) {
            bool text = openedFile->form->showingText();
            openedFile->form->load(openedFile->path, openedFile->editor, &error);
            openedFile->form->showText(openedFile->editor, text);
          }
        for (auto& file : files_) lint(*file);
      };
    };
    std::vector<MenuItem> formats, tvItems, emulatorItems, illegal;
    // AUTO, then a submenu per category with its formats in name order (8K before 16K)
    formats.push_back(item(T("Detected by the emulator (AUTO)"), "", set([](Project& project) { project.format = "AUTO"; }), true,
                           project_->format == "AUTO"));
    formats.push_back(MenuItem::sep());
    for (auto& [category, title] : formatCategories()) {
      std::vector<const CartFormat*> categoryFormats;
      for (auto& cartFormat : kb_.formats())
        if (cartFormat.category == category) categoryFormats.push_back(&cartFormat);
      std::sort(categoryFormats.begin(), categoryFormats.end(), [](auto* left, auto* right) { return naturalLess(left->name, right->name); });
      std::vector<MenuItem> items;
      bool current = false;
      for (auto* cartFormat : categoryFormats) {
        MenuItem formatItem = item(cartFormat->id, "", set([id = cartFormat->id](Project& project) {
          project.format = id;
          if (!formatSupported(project.emulator, id)) project.emulator = "gopher2600";  // ACE: only Gopher2600 runs it
        }), true, project_->format == cartFormat->id);
        if (!native) formatItem.hint = cartFormat->name;  // on the right
        else formatItem.label = cartFormat->name.find(cartFormat->id) != std::string::npos ? cartFormat->name : cartFormat->id + " — " + cartFormat->name;
        current = current || formatItem.checked;
        items.push_back(formatItem);
      }
      if (items.empty()) continue;
      MenuItem categoryItem = sub(title, items);
      categoryItem.checked = current;  // marks the category of the project's format
      formats.push_back(categoryItem);
    }
    auto tvIds = tvStandardsFor(project_->language);
    tvIds.push_back("AUTO");
    if (std::find(tvIds.begin(), tvIds.end(), project_->tv) == tvIds.end()) tvIds.push_back(project_->tv);  // set by hand
    for (auto& tv : tvIds) {
      MenuItem tvItem = item(tvLabel(tv), "", set([tv](Project& project) { project.tv = tv; }), true, project_->tv == tv);
      if (!tvSupported(project_->emulator, tv)) {
        Tool* tool = tools_.get(project_->emulator);
        tvItem.hint = F("not in {0}", {tool ? tool->name : project_->emulator});
      }
      tvItems.push_back(tvItem);
    }
    for (auto emulatorId : {"gopher2600", "stella"}) {
      Tool* tool = tools_.get(emulatorId);
      MenuItem emulatorItem = item(tool->name, "", set([emulatorId](Project& project) { project.emulator = emulatorId; }), true, project_->emulator == emulatorId);
      emulatorItem.hint = !formatSupported(emulatorId, project_->format) ? F("no {0}", {project_->format}) : tool->found() ? T("found") : T("not found");
      emulatorItems.push_back(emulatorItem);
    }
    const IllegalOpcodes policies[] = {IllegalOpcodes::Allow, IllegalOpcodes::StableOnly, IllegalOpcodes::Warn, IllegalOpcodes::Forbid};
    const char* names[] = {N_("Allow"), N_("Stable only"), N_("Warn"), N_("Forbid")};
    for (int i = 0; i < 4; i++)
      illegal.push_back(item(T(names[i]), "", set([policy = policies[i]](Project& project) { project.illegal = policy; }), true, project_->illegal == policies[i]));
    buildMenu.push_back(MenuItem::sep());
    buildMenu.push_back(sub(T("Format"), formats));
    buildMenu.push_back(sub(T("TV standard"), tvItems));
    buildMenu.push_back(sub(T("Emulator"), emulatorItems));
    buildMenu.push_back(item(T("Emulator options..."), "", [this] { showEmuOptions_ = true; }));
    buildMenu.push_back(item(T("Project settings..."), "", [this] { projectSettings(); }));
    buildMenu.push_back(sub(T("Illegal opcodes"), illegal));
  }
  bar.push_back(sub(T("Build"), buildMenu));

  // View
  std::vector<MenuItem> themeMenu;
  for (int pass = 0; pass < 2; pass++) {
    bool first = true;
    for (auto& themeEntry : themes_.themes())
      if (themeEntry.builtIn == (pass == 0)) {
        if (pass == 1 && first) themeMenu.push_back(MenuItem::sep());  // the user's themes after the built-in ones
        first = false;
        themeMenu.push_back(item(theme::displayName(themeEntry), "", [this, id = themeEntry.id] { useTheme(id); }, true, themeEntry.id == themeId_));
      }
  }
  themeMenu.push_back(MenuItem::sep());
  themeMenu.push_back(item(T("Edit themes..."), "", [this] { themeEditor_.open = true; }));
  bar.push_back(sub(T("View"), {
      item(T("Show cycle column"), "", [this] {
        showCycles_ = !showCycles_;
        for (auto& openedFile : files_) openedFile->editor.setGutterWidthChars(gutterChars(*openedFile));
        saveSettings();
      }, true, showCycles_),
      item(T("Addresses instead of cycles"), "", [this] { switchCycleColumn(); }, showCycles_, columnAddresses_),
      item(T("Show 6502 page separators"), "", [this] {
        showPages_ = !showPages_;
        for (auto& openedFile : files_) updatePageSeparators(*openedFile);
        saveSettings();
      }, true, showPages_),
      MenuItem::sep(),
      item(T("Zoom in"), "Ctrl++", [this] { zoomEditor(1); }, zoom_ < 3.0f),
      item(T("Zoom out"), "Ctrl+-", [this] { zoomEditor(-1); }, zoom_ > 0.5f),
      item(F("Actual size ({0} %)", {std::to_string((int)(zoom_ * 100 + 0.5f))}), "Ctrl+0", [this] { zoomEditor(0); },
           zoom_ != 1.0f),
      MenuItem::sep(),
      sub(T("Theme"), themeMenu),
      item(T("Documentation"), "", [this] { showDocs_ = !showDocs_; }, true, showDocs_),
      item(T("Assistant"), "", [this] { showAssistant_ = !showAssistant_; }, assistant_.enabled(), showAssistant_ && assistant_.enabled()),
      item(T("Reset layout"), "", [this] { resetLayout_ = true; }),
  }));
  if (!native) bar.push_back(sub(T("Settings"), settings));

  // Help
  std::vector<MenuItem> help = {item(T("Open documentation"), "F1", [this] { focusDocs_ = true; })};
  if (!native) help.push_back(item(T("About VCSIDE"), "", [this] { showAbout_ = true; }));
  bar.push_back(sub(T("Help"), help));
  return bar;
}

void App::menuBar() {
  if (nativeMenu_) {
    if (active_)
      for (auto& action : nativeMenuTakeActions()) action();
    return;
  }
  if (!ImGui::BeginMainMenuBar()) return;
  drawImGuiMenus(buildMenuBar(false));
  // status on the right of the menu bar
  if (!lastStatus_.empty()) {
    float statusWidth = ImGui::CalcTextSize(lastStatus_.c_str()).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - statusWidth - 20);
    ImGui::TextDisabled("%s", lastStatus_.c_str());
  }
  ImGui::EndMainMenuBar();
}

void App::dockspace() {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  // leave room for the status bar
  float statusH = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusH));
  ImGui::SetNextWindowViewport(viewport->ID);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                           ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::Begin("##root", nullptr, flags);
  ImGui::PopStyleVar(2);
  ImGuiID id = ImGui::GetID("MainDock");
  if (resetLayout_ || ImGui::DockBuilderGetNode(id) == nullptr) {
    resetLayout_ = false;
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, ImGui::GetContentRegionAvail());
    ImGuiID center = id, left, right, bottom;
    left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
    right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
    bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, nullptr, &center);
    ImGui::DockBuilderDockWindow("###project", left);
    ImGui::DockBuilderDockWindow("###docs", right);
    ImGui::DockBuilderDockWindow("###output", bottom);
    ImGui::DockBuilderDockWindow("###problems", bottom);
    ImGui::DockBuilderDockWindow("###space", bottom);
    ImGui::DockBuilderDockWindow("###editors", center);
    ImGui::DockBuilderFinish(id);
  }
  ImGui::DockSpace(id, ImVec2(0, 0), ImGuiDockNodeFlags_None);
  ImGui::End();
  statusBar();
}

void App::statusBar() {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  float barHeight = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - barHeight));
  ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, barHeight));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 3));
  ImGui::Begin("##status", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
  ImGui::PopStyleVar();
  if (nativeMenu_ && !lastStatus_.empty()) {
    ImGui::TextDisabled("%s  |", lastStatus_.c_str());
    ImGui::SameLine();
  }
  if (project_) {
    ImGui::Text("%s  |  %s  %s  %s", project_->name.c_str(), project_->format.c_str(), tvLabel(project_->tv).c_str(),
                project_->emulator.c_str());
    ImGui::SameLine();
  }
  if (activeFile_ >= 0) {
    auto& openedFile = *files_[activeFile_];
    auto& textEditor = openedFile.editor;
    ImGui::Text("|  %s %d, %s %d", T("Line"), textEditor.cursor().line + 1, T("Col"), textEditor.cursor().col + 1);
    if (openedFile.asmSyntax && textEditor.hasSelection()) {
      auto [firstLine, lastLine] = textEditor.selectedLines();
      std::vector<std::string> selectedLines(textEditor.lines().begin() + firstLine, textEditor.lines().begin() + lastLine + 1);
      BlockCycles blockCycles = countBlock(kb_, selectedLines, [this](const std::string& name) { return lookupSymbol(name); });
      ImGui::SameLine();
      std::string cycleText = std::to_string(blockCycles.min) + (blockCycles.max != blockCycles.min ? "-" + std::to_string(blockCycles.max) : "");
      std::string message = "|  " + F("{0} lines: {1} cycles, {2} bytes", {std::to_string(blockCycles.lines), cycleText, std::to_string(blockCycles.bytes)});
      ImGui::TextColored(theme::colv(theme::Role::MsgHighlight), "%s", message.c_str());
      ImGui::SameLine();
      char scanlines[48];
      snprintf(scanlines, sizeof scanlines, "%.2f-%.2f", blockCycles.min / 76.0, blockCycles.max / 76.0);
      std::string extra = F("({0} scanlines)", {scanlines});
      if (blockCycles.unknown) extra += "  " + F("{0} unknown", {std::to_string(blockCycles.unknown)});
      if (blockCycles.guessed) extra += "  " + F("{0} guessed (~)", {std::to_string(blockCycles.guessed)});
      ImGui::TextDisabled("%s", extra.c_str());
    }
  }
  if (space_ || elfSpace_) {
    std::string summary = space_ ? space_->summary() : elfSpace_->summary();
    bool error = false, warn = false;
    for (auto& diagnostic : space_ ? space_->diagnostics : elfSpace_->diagnostics)
      error |= diagnostic.severity == Diagnostic::Error, warn |= diagnostic.severity == Diagnostic::Warning;
    float summaryWidth = ImGui::CalcTextSize(summary.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowWidth() - summaryWidth - 12));
    ImVec4 colour = theme::colv(error ? theme::Role::MsgError : warn ? theme::Role::MsgWarning : theme::Role::UiTextDim);
    ImGui::TextColored(colour, "%s", summary.c_str());
  }
  ImGui::End();
}

void App::projectPanel() {
  char title[64];
  snprintf(title, sizeof title, "%s###project", T("Project"));
  if (!theme::beginPanel(title)) {
    theme::endPanel();
    return;
  }
  if (folder_.empty()) {  // the welcome page, in the editor's place, has the ways in
    ImGui::TextDisabled("%s", T("No project open."));
    theme::endPanel();
    return;
  }
  // A folder that can be built and has no project file yet (or a detected one not saved)
  if (!project_ || projectUnsaved_) {
    if (!projectUnsaved_ && detectedFor_ != folder_) {
      detectedFor_ = folder_;
      detected_.reset();
      if (Project candidate; Project::detect(folder_, &candidate)) detected_ = candidate;
    }
    const Project* found = projectUnsaved_ ? &*project_ : detected_ ? &*detected_ : nullptr;
    if (found) {
      ImGui::PushTextWrapPos(0);
      if (!found->buildCommand.empty()) ImGui::TextUnformatted(F("This folder builds with \"{0}\".", {found->buildCommand}).c_str());
      else ImGui::TextUnformatted(F("This folder has a program in {0}: {1}.", {languageName(found->language), found->main}).c_str());
      ImGui::TextDisabled("%s", projectUnsaved_ ? T("Built with what was found, without a project file: one keeps these settings.")
                                                : F("F7 builds it and F5 opens {0} in the emulator; a project file keeps the settings.", {found->output}).c_str());
      ImGui::PopTextWrapPos();
      if (ImGui::Button(T("Create a project for it"))) {
        if (!projectUnsaved_) project_ = *found, projectUnsaved_ = true;
        detectedFor_.clear(), detected_.reset();
        createProjectFile();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", F("Writes {0}. Its settings (main source, ROM, format...) can be changed afterwards in Build > Project settings...",
                                  {fs::path(found->file).filename().string()}).c_str());
      ImGui::Separator();
    }
  }
  updateGitIgnored();
  // The tree, drawn row by row: chevron, icon, name; guides for each level.
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  float fontH = ImGui::GetTextLineHeight(), rowH = std::round(fontH * 1.45f), indentW = std::round(fontH * 0.9f);
  float iconSize = std::round(fontH * 0.95f);
  float left = ImGui::GetWindowPos().x, right = left + ImGui::GetWindowWidth();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().WindowPadding.y * 0.5f);
  auto isOpen = [&](const std::string& path, bool byDefault) { return treeToggled_.count(path) ? !byDefault : byDefault; };
  auto toggle = [&](const std::string& path) {
    if (!treeToggled_.erase(path)) treeToggled_.insert(path);
  };
  // One row; true if clicked. `depth` 0 is the project's own row.
  auto row = [&](const std::string& id, int depth, const std::string& name, files::Kind kind, bool dir, bool open,
                 bool active, bool dimmed) {
    ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
    ImGui::PushID(id.c_str());
    bool clicked = ImGui::InvisibleButton("row", ImVec2(std::max(1.0f, right - rowOrigin.x), rowH));
    bool hover = ImGui::IsItemHovered();
    if (theme::beginContextMenu("##rowmenu")) {  // right click: what is usually done to a file or a folder
      bool isRoot = depth == 0;
      fs::path where(id);
      std::string inFolder = dir ? id : where.parent_path().string();
      if (!dir && ImGui::MenuItem(T("Open"))) openFile(id);
      if (!dir) ImGui::Separator();
      if (ImGui::MenuItem(T("New file..."))) startFileOp(FileOp::NewFile, inFolder);
      if (ImGui::MenuItem(T("New folder..."))) startFileOp(FileOp::NewFolder, inFolder);
      if (!isRoot) {
        ImGui::Separator();
        if (ImGui::MenuItem(T("Rename..."))) startFileOp(FileOp::Rename, id, where.filename().string());
        if (ImGui::MenuItem(T("Duplicate"))) {
          std::string copy, error;
          if (fileops::duplicate(id, &copy, &error)) {
            if (!dir) openFile(copy);
          } else {
            startFileOp(FileOp::Message, id), fileOp_.error = error;
          }
        }
        if (ImGui::MenuItem(T("Delete..."))) startFileOp(FileOp::Delete, id, where.filename().string());
      }
      ImGui::Separator();
      if (ImGui::MenuItem(T("Copy path"))) ImGui::SetClipboardText(fs::absolute(where).string().c_str());
      if (!isRoot && ImGui::MenuItem(T("Copy relative path"))) {
        std::error_code relativeError;
        fs::path relative = fs::relative(where, folder_, relativeError);
        ImGui::SetClipboardText((relativeError ? where : relative).generic_string().c_str());
      }
      if (ImGui::MenuItem(T("Show in the file manager"))) SDL_OpenURL(("file://" + fs::absolute(fs::path(inFolder)).string()).c_str());
      theme::endMenuPopup();
    }
    ImGui::PopID();
    if (active) drawList->AddRectFilled(ImVec2(left, rowOrigin.y), ImVec2(right, rowOrigin.y + rowH), theme::col(theme::Role::FilesRowSelected));
    else if (hover) drawList->AddRectFilled(ImVec2(left, rowOrigin.y), ImVec2(right, rowOrigin.y + rowH), theme::col(theme::Role::FilesRowHover));
    float x = rowOrigin.x + depth * indentW, cy = rowOrigin.y + rowH * 0.5f;
    for (int level = 1; level <= depth; level++) {  // a guide under each open folder above
      float gx = std::round(rowOrigin.x + (level - 1) * indentW + indentW * 0.5f) + 0.5f;
      drawList->AddLine(ImVec2(gx, rowOrigin.y), ImVec2(gx, rowOrigin.y + rowH), theme::col(theme::Role::FilesGuide));
    }
    ImU32 text = theme::col(theme::Role::UiText);
    int firstVertex = drawList->VtxBuffer.Size;  // what follows (chevron, icon, name) fades when dimmed
    if (dir) {  // chevron: > closed, v open
      float chevronSize = fontH * 0.22f, cx = x + indentW * 0.5f, thickness = std::max(1.0f, fontH * 0.09f);
      ImU32 chevronColour = theme::col(theme::Role::UiTextDim);
      if (open) drawList->AddPolyline(std::array<ImVec2, 3>{ImVec2(cx - chevronSize, cy - chevronSize * 0.5f), ImVec2(cx, cy + chevronSize * 0.5f), ImVec2(cx + chevronSize, cy - chevronSize * 0.5f)}.data(), 3, chevronColour, 0, thickness);
      else drawList->AddPolyline(std::array<ImVec2, 3>{ImVec2(cx - chevronSize * 0.5f, cy - chevronSize), ImVec2(cx + chevronSize * 0.5f, cy), ImVec2(cx - chevronSize * 0.5f, cy + chevronSize)}.data(), 3, chevronColour, 0, thickness);
    }
    x += indentW;
    if (depth > 0 || !dir) {
      files::drawIcon(drawList, ImVec2(x, std::round(cy - iconSize * 0.5f)), iconSize, kind, open);
      x += iconSize + fontH * 0.4f;
    }
    drawList->AddText(ImVec2(x, std::round(cy - fontH * 0.5f)), text, name.c_str());
    if (dimmed) {  // build output and what Git ignores: the whole row at reduced opacity, in any theme
      for (int vertex = firstVertex; vertex < drawList->VtxBuffer.Size; vertex++) {
        ImU32& colour = drawList->VtxBuffer[vertex].col;
        ImU32 alpha = (colour >> IM_COL32_A_SHIFT) & 0xFF;
        colour = (colour & ~IM_COL32_A_MASK) | ((ImU32)(alpha * 0.45f) << IM_COL32_A_SHIFT);
      }
    }
    if (hover && ImGui::CalcTextSize(name.c_str()).x > right - x) ImGui::SetTooltip("%s", name.c_str());
    return clicked;
  };
  std::function<void(const fs::path&, int, bool)> tree = [&](const fs::path& dir, int depth, bool dimmed) {
    std::vector<fs::directory_entry> entries;
    std::error_code errorCode;
    for (auto& entry : fs::directory_iterator(dir, errorCode)) {
      std::string name = entry.path().filename().string();
      if (name.empty() || name == ".git") continue;  // every file shows, but not Git's own folder
      entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](auto& first, auto& second) {
      if (first.is_directory() != second.is_directory()) return first.is_directory();
      return lower(first.path().filename().string()) < lower(second.path().filename().string());
    });
    for (auto& entry : entries) {
      std::string name = entry.path().filename().string(), entryPath = entry.path().string();
      bool isDirectory = entry.is_directory();
      bool ignored = gitIgnored_.count(entryPath) > 0;
      if (isDirectory) {
        bool sub = dimmed || ignored || name == "build";  // build output and what Git ignores: shown dimmed
        bool open = isOpen(entryPath, false);
        if (row(entryPath, depth, name, files::Kind::Folder, true, open, false, sub)) toggle(entryPath), open = !open;
        if (open) tree(entry.path(), depth + 1, sub);
      } else {
        bool active = activeFile_ >= 0 && files_[activeFile_]->path == entryPath;
        if (row(entryPath, depth, name, files::kindOf(name, false), false, false, active, dimmed || ignored)) {
          if (entry.path().extension() == ".vcsproj" && !(project_ && fs::weakly_canonical(project_->file) == fs::weakly_canonical(entryPath))) openProject(entryPath);
          else if (entry.path().extension() == ".vcsproj") projectSettings();
          else if (files::kindOf(name, false) != files::Kind::Rom) openFile(entryPath);
        }
        // a ROM runs on a double click (the row's button, still the last item, reports clicks on release)
        if (files::kindOf(name, false) == files::Kind::Rom && ImGui::IsItemHovered()) {
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) runRom(entryPath);
          else ImGui::SetTooltip("%s\n%s", name.c_str(), T("Double click to run it."));
        }
      }
    }
  };
  // the project's row: its name in capitals, like a section title
  std::string rootName = fs::path(folder_).filename().string();
  for (auto& character : rootName) character = (char)std::toupper((unsigned char)character);
  bool rootOpen = isOpen(folder_, true);
  if (row(folder_, 0, rootName, files::Kind::Folder, true, rootOpen, false, false)) toggle(folder_), rootOpen = !rootOpen;
  if (rootOpen) tree(folder_, 1, false);
  ImGui::PopStyleVar();
  theme::endPanel();
}

// Asks Git (in the background) which files and folders of the project it ignores. Without
// Git, or outside a repository, nothing is ignored.
void App::updateGitIgnored() {
  if (gitJob_ && gitJob_->finished()) {
    gitIgnored_.clear();
    if (gitJob_->exitCode() == 0 && gitFolder_ == folder_) {
      std::string output = gitJob_->output();
      size_t start = 0;
      if (output.rfind("> ", 0) == 0) start = output.find('\n') + 1;  // the command line Job puts first
      for (size_t end; (end = output.find('\0', start)) != std::string::npos; start = end + 1) {
        std::string relative = output.substr(start, end - start);
        while (!relative.empty() && (relative.back() == '/' || relative.back() == '\\')) relative.pop_back();  // a folder
        if (!relative.empty()) gitIgnored_.insert((fs::path(folder_) / fs::path(relative)).make_preferred().string());
      }
    }
    gitJob_.reset();
  }
  if (gitJob_ || folder_.empty()) return;
  if (gitFolder_ != folder_) gitIgnored_.clear(), gitAskedAt_ = -100;
  double now = ImGui::GetTime();
  if (now - gitAskedAt_ < 3) return;
  gitAskedAt_ = now;
  gitFolder_ = folder_;
  static const std::string git = findInPath("git");
  if (git.empty()) return;
  gitJob_ = std::make_unique<Job>();
  // the paths come relative to the folder, with ignored folders as one entry ending in /
  if (!gitJob_->start({git, "ls-files", "--others", "--ignored", "--exclude-standard", "--directory", "-z"}, folder_)) gitJob_.reset();
}

// The whole window, with no title or panels, for the welcome page.
void App::welcomeScreen() {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::SetNextWindowViewport(viewport->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::colv(theme::Role::UiBackground));
  ImGui::Begin("##welcome", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMove);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  welcome();
  ImGui::End();
}

// No project and no file: the icon, the ways to start and the recent projects.
void App::welcome() {
  using theme::em;
  ImVec2 avail = ImGui::GetContentRegionAvail();
  float contentWidth = std::min(avail.x - em(48), em(420));
  float x0 = std::max(em(24), (avail.x - contentWidth) * 0.5f);
  int shown = std::min((int)recent_.size(), 8);
  float rowH = ImGui::GetTextLineHeight() * 2.6f;
  float blockH = em(96) + em(150) + (shown ? em(44) + shown * rowH : 0);
  ImGui::SetCursorPos(ImVec2(x0, std::max(em(24), (avail.y - blockH) * 0.4f)));
  ImGui::BeginGroup();
  if (unsigned texture = iconTexture()) {
    ImGui::Image((ImTextureID)(intptr_t)texture, ImVec2(em(72), em(72)));
    ImGui::SameLine(0, em(16));
  }
  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(0, em(10)));
  ImGui::SetWindowFontScale(1.7f);
  ImGui::TextUnformatted("VCSIDE");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::TextDisabled("%s %s", T("Version"), VCSIDE_VERSION);
  ImGui::EndGroup();
  ImGui::Dummy(ImVec2(0, em(18)));

  // the ways in: links, one per line
  auto link = [&](const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colv(theme::Role::UiLink));
    bool clicked = ImGui::Selectable(label, false, 0, ImVec2(contentWidth, 0));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
  };
  if (link(T("New project..."))) showNewProject_ = true;
  if (link(T("Open project..."))) {
    static const SDL_DialogFileFilter filters[] = {{"VCSIDE project", "vcsproj"}};
    SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "project"}, window_, filters, 1, nullptr, false);
  }
  if (link(T("Open folder...")))
    showFolderDialog(dialogCallback, new DialogCtx{this, "folder"}, window_, nullptr);

  if (shown) {
    ImGui::Dummy(ImVec2(0, em(18)));
    ImGui::TextDisabled("%s", T("Recent"));
    ImGui::Dummy(ImVec2(0, em(4)));
    const char* home = std::getenv("HOME");
    std::string homeDir = home ? home : "";
    for (int i = 0; i < shown; i++) {
      const std::string& recentPath = recent_[i];
      fs::path recentFile(recentPath);
      bool isProject = lower(recentFile.extension().string()) == ".vcsproj";
      std::string name = isProject ? recentFile.stem().string() : recentFile.filename().string();
      std::string where = (isProject ? recentFile.parent_path() : recentFile).string();
      if (!homeDir.empty() && where.rfind(homeDir, 0) == 0) where = "~" + where.substr(homeDir.size());
      std::error_code errorCode;
      bool exists = fs::exists(recentFile, errorCode);
      ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
      ImGui::PushID(i);
      ImGui::BeginDisabled(!exists);
      bool clicked = ImGui::Selectable("##recent", false, 0, ImVec2(contentWidth, rowH));
      ImGui::EndDisabled();
      ImGui::PopID();
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s", recentPath.c_str());
      }
      ImDrawList* drawList = ImGui::GetWindowDrawList();
      float lineHeight = ImGui::GetTextLineHeight(), iconSize = std::round(lineHeight * 1.1f);
      files::drawIcon(drawList, ImVec2(rowOrigin.x + em(4), rowOrigin.y + (rowH - iconSize) * 0.5f), iconSize,
                      isProject ? files::Kind::Project : files::Kind::Folder);
      float textX = rowOrigin.x + em(4) + iconSize + em(10), textY = rowOrigin.y + (rowH - 2 * lineHeight) * 0.5f;
      drawList->AddText(ImVec2(textX, textY), theme::col(exists ? theme::Role::UiText : theme::Role::UiTextDim), name.c_str());
      std::string subtitle = exists ? where : where + "  " + T("(not found)");
      // too long: keep its end, the part that tells projects apart
      float room = rowOrigin.x + contentWidth - textX - em(4);
      while (subtitle.size() > 4 && ImGui::CalcTextSize(subtitle.c_str()).x > room) {
        size_t cut = subtitle.rfind("\xE2\x80\xA6", 0) == 0 ? 3 : 0;
        size_t next = cut + 1;
        while (next < subtitle.size() && ((unsigned char)subtitle[next] & 0xC0) == 0x80) next++;
        subtitle = "\xE2\x80\xA6" + subtitle.substr(next);
      }
      drawList->AddText(ImVec2(textX, textY + lineHeight), theme::col(theme::Role::UiTextDim), subtitle.c_str());
      if (clicked && exists) openPath(recentPath);
    }
  }
  ImGui::EndGroup();
}

void App::editorsPanel() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  bool open = theme::beginPanel("Editor###editors", nullptr, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  if (!open) {
    theme::endPanel();
    return;
  }
  if (files_.empty() && folder_.empty()) {
    welcome();
    theme::endPanel();
    return;
  }
  if (files_.empty()) {
    ImGui::SetCursorPos(ImVec2(20, 20));
    ImGui::TextDisabled("VCSIDE %s", VCSIDE_VERSION);
    ImGui::SetCursorPosX(20);
    ImGui::TextDisabled("F7 %s   F5 %s   F6 %s   F1 %s", T("Build"), T("Build and run"), T("Build and debug"), T("Documentation"));
    theme::endPanel();
    return;
  }
  if (ImGui::BeginTabBar("files", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (int i = 0; i < (int)files_.size(); i++) {
      auto& openedFile = *files_[i];
      bool keep = true;
      std::string label = fs::path(openedFile.path).filename().string() + (openedFile.dirty() ? " *" : "") + "###" + openedFile.path;
      ImGuiTabItemFlags tabFlags = openedFile.requestFocus ? ImGuiTabItemFlags_SetSelected : 0;
      if (openedFile.dirty()) tabFlags |= ImGuiTabItemFlags_UnsavedDocument;
      if (ImGui::BeginTabItem(label.c_str(), &keep, tabFlags)) {
        activeFile_ = i;
        if (openedFile.requestFocus) {
          ImGui::SetKeyboardFocusHere();
          openedFile.requestFocus = false;
        }
        if (openedFile.image) {
          ImGui::BeginChild("##image", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
          openedFile.image->render();
          ImGui::EndChild();
        } else if (openedFile.form) {
          ImGui::BeginChild("##form", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar);
          openedFile.form->render(openedFile.editor, editorFont(), kb_, tools_);
          ImGui::EndChild();
        } else if (openedFile.music) {
          ImGui::BeginChild("##music", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar);
          openedFile.music->render(project_ ? project_->tv : "NTSC");
          ImGui::EndChild();
        } else if (openedFile.gfx) {
          ImGui::BeginChild("##gfx", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar);
          openedFile.gfx->render(gfxTv(), gfxProfile());
          ImGui::EndChild();
        } else if (openedFile.editor.render("##code", editorFont())) {
          lint(openedFile);
        }
        if (int zoomDelta = openedFile.editor.takeZoomSteps()) zoomEditor(zoomDelta);
        if (openedFile.editor.takeBreakpointsChanged()) {
          breakpoints_[openedFile.path] = openedFile.editor.breakpoints();
          saveUserState();
        }
        ImGui::EndTabItem();
      }
      if (!keep) closeRequest_ = i;
    }
    ImGui::EndTabBar();
  }
  theme::endPanel();
}

void App::startFileOp(FileOp::Kind kind, const std::string& target, const std::string& name) {
  fileOp_ = FileOp();
  fileOp_.kind = kind, fileOp_.target = target, fileOp_.name = name;
}

// A file or folder was renamed: the open files under it, and the folders opened in the tree, follow it.
void App::pathsMoved(const std::string& from, const std::string& to) {
  auto moved = [&](const std::string& path) -> std::string {
    if (path == from) return to;
    std::string prefix = from + std::string(1, fs::path::preferred_separator);
    if (path.rfind(prefix, 0) == 0) return to + path.substr(from.size());
    return path;
  };
  for (auto& file : files_) file->path = moved(file->path);
  std::set<std::string> toggled;
  for (auto& path : treeToggled_) toggled.insert(moved(path));
  treeToggled_ = toggled;
  if (project_ && project_->file == from) project_->file = to;
}

// A file or folder was deleted: the files open from it are closed, without asking (the dialog said so).
void App::pathsGone(const std::string& path) {
  std::string prefix = path + std::string(1, fs::path::preferred_separator);
  for (size_t i = files_.size(); i-- > 0;)
    if (files_[i]->path == path || files_[i]->path.rfind(prefix, 0) == 0) {
      files_.erase(files_.begin() + i);
      if (activeFile_ >= (int)i) activeFile_--;
    }
  if (activeFile_ < 0 && !files_.empty()) activeFile_ = 0;
  if (files_.empty()) activeFile_ = -1;
  for (auto it = treeToggled_.begin(); it != treeToggled_.end();)
    it = (*it == path || it->rfind(prefix, 0) == 0) ? treeToggled_.erase(it) : std::next(it);
}

void App::fileDialogs() {
  if (fileOp_.kind == FileOp::None) return;
  if (!fileOp_.opened) ImGui::OpenPopup("###fileop"), fileOp_.opened = true;
  const char* heading = fileOp_.kind == FileOp::NewFile ? T("New file") : fileOp_.kind == FileOp::NewFolder ? T("New folder") : fileOp_.kind == FileOp::Rename ? T("Rename")
                        : fileOp_.kind == FileOp::Delete ? T("Delete") : T("File");
  char title[96];
  snprintf(title, sizeof title, "%s###fileop", heading);
  if (!theme::beginModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (!ImGui::IsPopupOpen("###fileop")) fileOp_ = FileOp();  // closed from outside (Escape, another window)
    return;
  }
  bool close = false;
  auto errorLine = [&] {
    if (!fileOp_.error.empty()) ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", fileOp_.error.c_str());
  };
  if (fileOp_.kind == FileOp::Delete) {
    bool isFolder = fs::is_directory(fileOp_.target);
    ImGui::PushTextWrapPos(theme::em(420));
    ImGui::TextUnformatted(F(isFolder ? "Delete the folder \"{0}\" and everything in it?" : "Delete \"{0}\"?", {fileOp_.name}).c_str());
    ImGui::TextDisabled("%s", T("This cannot be undone. Open files in it are closed without saving."));
    ImGui::PopTextWrapPos();
    errorLine();
    if (theme::primaryButton(T("Delete"))) {
      std::string error;
      if (fileops::remove(fileOp_.target, &error)) pathsGone(fileOp_.target), close = true;
      else fileOp_.error = error;
    }
    ImGui::SameLine();
    if (theme::dialogButton(T("Cancel")) || theme::dialogEscape()) close = true;
  } else if (fileOp_.kind == FileOp::Message) {
    ImGui::PushTextWrapPos(theme::em(420));
    errorLine();
    ImGui::PopTextWrapPos();
    if (theme::primaryButton(T("OK")) || theme::dialogEscape()) close = true;
  } else {
    if (fileOp_.kind != FileOp::Rename) ImGui::TextDisabled("%s", F("In {0}", {fs::path(fileOp_.target).filename().string()}).c_str());
    ImGui::SetNextItemWidth(theme::em(300));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##name", &fileOp_.name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    errorLine();
    bool ok = theme::primaryButton(T("OK")) || enter;
    ImGui::SameLine();
    if (theme::dialogButton(T("Cancel")) || theme::dialogEscape()) close = true;
    if (ok && !close) {
      std::string result, error;
      bool done = fileOp_.kind == FileOp::NewFile     ? fileops::createFile(fileOp_.target, fileOp_.name, &result, &error)
                  : fileOp_.kind == FileOp::NewFolder ? fileops::createFolder(fileOp_.target, fileOp_.name, &result, &error)
                                                      : fileops::rename(fileOp_.target, fileOp_.name, &result, &error);
      if (done) {
        if (fileOp_.kind == FileOp::NewFile) openFile(result);
        else if (fileOp_.kind == FileOp::Rename && result != fileOp_.target) pathsMoved(fileOp_.target, result);
        if (fileOp_.kind != FileOp::Rename && fileOp_.target != folder_ && !treeToggled_.count(fileOp_.target)) treeToggled_.insert(fileOp_.target);  // the folder it went into shows it open
        close = true;
      } else {
        fileOp_.error = error;
      }
    }
  }
  if (close) {
    fileOp_ = FileOp();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void App::closeDialogs() {
  if (closeRequest_ >= 0 && closeRequest_ < (int)files_.size()) {
    if (!files_[closeRequest_]->dirty()) {
      files_.erase(files_.begin() + closeRequest_);
      activeFile_ = std::min(activeFile_, (int)files_.size() - 1);
      closeRequest_ = -1;
    } else {
      ImGui::OpenPopup("###unsaved");
    }
  }
  char title[96];
  snprintf(title, sizeof title, "%s###unsaved", T("Unsaved changes"));
  if (theme::beginModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(T("Save changes before closing?"));
    if (closeRequest_ >= 0) ImGui::TextDisabled("%s", files_[closeRequest_]->path.c_str());
    int action = 0;
    if (theme::primaryButton(T("Save"))) action = 1;
    ImGui::SameLine();
    if (theme::dialogButton(T("Don't save"))) action = 2;
    ImGui::SameLine();
    if (theme::dialogButton(T("Cancel")) || theme::dialogEscape()) action = 3;
    if (action) {
      if (closeRequest_ >= 0) {
        if (action == 1) saveFile(*files_[closeRequest_]);
        if (action != 3) {
          files_.erase(files_.begin() + closeRequest_);
          activeFile_ = std::min(activeFile_, (int)files_.size() - 1);
        }
      }
      closeRequest_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (quitConfirm_) ImGui::OpenPopup("###quit");
  snprintf(title, sizeof title, "%s###quit", T("Unsaved changes"));
  if (theme::beginModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    quitConfirm_ = false;
    ImGui::TextUnformatted(T("Save changes before closing?"));
    auto done = [&] {
      if (closeProjectAfterConfirm_) doCloseProject();
      else quit_ = true;
      closeProjectAfterConfirm_ = false;
      ImGui::CloseCurrentPopup();
    };
    if (theme::primaryButton(T("Save all"))) saveAll(), done();
    ImGui::SameLine();
    if (theme::dialogButton(T("Don't save"))) done();
    ImGui::SameLine();
    if (theme::dialogButton(T("Cancel")) || theme::dialogEscape()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

// Never throws: the project's folder may be gone (moved, deleted) while the window is open.
static std::string relativeToProject(const std::string& projectDir, const std::string& path) {
  std::error_code ignored;
  fs::path relative = fs::relative(path, projectDir, ignored);
  return ignored || relative.empty() ? path : relative.generic_string();
}

void App::assistantPanel() {
  if (!assistant_.enabled() || !showAssistant_) return;
  assistant::PanelContext context;
  context.kb = &kb_;
  if (project_) {
    context.projectDir = project_->dir;
    const char* language = project_->language == Language::BatariBasic ? "batari Basic" : project_->language == Language::ElfC ? "C for ELF" : project_->language == Language::AceC ? "C for ACE" : "DASM";
    context.projectInfo = std::string(language) + ", " + project_->format + ", " + project_->tv + ", " + project_->main;
    context.grounding.format = project_->format;
    context.grounding.tv = project_->tv;
    context.grounding.batariBasic = project_->language == Language::BatariBasic;
  }
  if (activeFile_ >= 0 && activeFile_ < (int)files_.size() && files_[activeFile_]->path.size() && !files_[activeFile_]->gfx &&
      !files_[activeFile_]->music && !files_[activeFile_]->image && !files_[activeFile_]->form) {
    OpenFile* active = files_[activeFile_].get();
    context.fileName = fs::path(active->path).filename().string();
    if (project_) context.fileRelative = relativeToProject(project_->dir, active->path);
    context.fileText = active->editor.text();
    context.cursorLine = active->editor.cursor().line + 1;
    context.selection = active->editor.selectedText();
    if (!context.selection.empty()) context.selectionFirst = active->editor.selectedLines().first + 1, context.selectionLast = active->editor.selectedLines().second + 1;
  }
  context.showDoc = [this](const std::string& word) { showDoc(word); };
  context.readOpenFile = [this](const std::string& relative, std::string* text) {  // as the editor holds it, unsaved changes included
    if (!project_) return false;
    for (auto& openedFile : files_)
      if (!openedFile->gfx && !openedFile->music && !openedFile->image && !openedFile->form && !openedFile->path.empty() &&
          relativeToProject(project_->dir, openedFile->path) == relative) {
        *text = openedFile->editor.text();
        return true;
      }
    return false;
  };
  std::string errors;
  for (auto& diagnostic : diagnostics_) {
    if (diagnostic.severity != Diagnostic::Error) continue;
    errors += fs::path(diagnostic.file).filename().string() + (diagnostic.line ? ":" + std::to_string(diagnostic.line) : "") + ": " + diagnostic.message + "\n";
    if (errors.size() > 3000) break;
  }
  context.errors = errors;
  context.building = building_;
  context.buildStatus = lastStatus_;
  if (space_) context.spaceReport = space_->summary();
  for (auto& [name, live] : liveSymbols_) {
    std::string relativeFile = project_ ? relativeToProject(project_->dir, live.file) : live.file;
    context.symbols += name + " " + relativeFile + ":" + std::to_string(live.symbol.line + 1) + "\n";
  }
  context.startBuild = [this] { build(0); };
  context.runEmulator = [this] { run(false); };
  context.applyEdit = [this](const std::string& path, const std::string& text, bool remove) {
    for (auto& openedFile : files_)
      if (openedFile->path == path) {
        if (openedFile->gfx || openedFile->music || openedFile->image || openedFile->form || remove) return false;
        openedFile->editor.selectAll();  // one undoable step in the editor
        openedFile->editor.typeText(text);
        return true;
      }
    std::error_code ignored;
    if (remove) return fs::remove(path, ignored);
    fs::create_directories(fs::path(path).parent_path(), ignored);
    std::ofstream out(path, std::ios::binary);
    out << text;
    return (bool)out;
  };
  assistant_.draw(context, mono_, &showAssistant_);
}

void App::outputPanel() {
  char title[64];
  snprintf(title, sizeof title, "%s###output", T("Output"));
  bool open = theme::beginPanel(title);
  bottomDock_ = ImGui::GetWindowDockID();
  if (open) {
    ImGui::PushFont(mono_);
    ImGui::TextUnformatted(output_.c_str());
    if (outputScroll_ || building_) ImGui::SetScrollHereY(1.0f);
    outputScroll_ = false;
    ImGui::PopFont();
  }
  theme::endPanel();
}

static void wrappedTooltip(const std::string& text) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

// CDFJ's driver: why VCSIDE has none, and what to do (New project and Problems).
static const char* kCdfjDriverHelp =
    N_("VCSIDE does not include CDFJ's ARM driver (cdfj_driver.bin, 2048 bytes). It is by Chris Walton, Fred Quimby and Darrell Spice Jr, published with SpiceWare's CDFJ tutorial on AtariAge, with no licence that lets VCSIDE pass it on. The project does not build until you add your own copy: Choose file... checks it and copies it into the project.");

void App::problemsPanel() {
  char title[64];
  snprintf(title, sizeof title, "%s (%d)###problems", T("Problems"), (int)diagnostics_.size());
  if (theme::beginPanel(title)) {
    if (ImGui::BeginTable("diag", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
      ImGui::TableSetupColumn(T("File"), ImGuiTableColumnFlags_WidthFixed, 200);
      ImGui::TableSetupColumn("");
      for (size_t i = 0; i < diagnostics_.size(); i++) {
        auto& diagnostic = diagnostics_[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImVec4 colour = theme::colv(diagnostic.severity == Diagnostic::Error ? theme::Role::MsgError : diagnostic.severity == Diagnostic::Warning ? theme::Role::MsgWarning : theme::Role::MsgInfo);
        ImGui::PushID((int)i);
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        bool clicked = ImGui::Selectable(diagnostic.severity == Diagnostic::Error ? T("error") : diagnostic.severity == Diagnostic::Warning ? T("warning") : T("info"), false,
                                         ImGuiSelectableFlags_SpanAllColumns);
        ImGui::PopStyleColor();
        if (assistant_.explainEnabled() && theme::beginContextMenu("explain")) {
          if (ImGui::MenuItem(T("Explain this error"))) {
            showAssistant_ = true;
            assistant_.explain("Explain this build error and how to fix it:\n" + fs::path(diagnostic.file).filename().string() +
                               (diagnostic.line ? ":" + std::to_string(diagnostic.line) : "") + ": " + diagnostic.message);
          }
          theme::endMenuPopup();
        }
        ImGui::PopID();
        ImGui::TableNextColumn();
        // CDFJ's missing driver: the button in the file's column, which a narrow panel still shows
        if (project_ && diagnostic.message.find(std::string(Project::kCdfjDriverFile) + " is missing") != std::string::npos) {
          if (ImGui::SmallButton(T("Choose file...")))  // the user's own copy: VCSIDE has none to offer
            SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "cdfjdriver"}, window_, nullptr, 0, nullptr, false);
          wrappedTooltip(T(kCdfjDriverHelp));
        } else if (!diagnostic.file.empty()) {
          std::string shown = diagnostic.file;
          std::error_code errorCode;
          if (project_ && fs::path(diagnostic.file).is_absolute()) {
            auto relative = fs::relative(diagnostic.file, project_->dir, errorCode);
            if (!errorCode && !relative.empty() && relative.native()[0] != '.') shown = relative.string();
          }
          if (diagnostic.line) ImGui::Text("%s:%d", shown.c_str(), diagnostic.line);
          else ImGui::TextUnformatted(shown.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(diagnostic.message.c_str());
        if (clicked && !diagnostic.file.empty() && project_) {
          std::string foundPath = findProjectFile(diagnostic.file);
          if (!foundPath.empty()) openFile(foundPath, std::max(1, diagnostic.line));
        }
      }
      ImGui::EndTable();
    }
  }
  theme::endPanel();
}

// From an address the emulator shows (PC, a break, a crash) to its source line.
// ---------------------------------------------------------------------------
// Emulator options (data/kb/emulators.json; stored in the project)

// Group names and the common value labels of emulators.json, for the catalogs.
[[maybe_unused]] static const char* kEmuWords[] = {
    N_("Console"), N_("Controllers"), N_("Video"), N_("Audio"), N_("Debugging"), N_("Randomize on reset"),
    N_("TIA chip"), N_("ARM (DPC+, CDFJ, ELF)"), N_("on"), N_("off"), N_("colour"), N_("black and white"),
    N_("auto"), N_("joystick"), N_("paddles"), N_("keypad"), N_("driving"), N_("yes"), N_("no"),
    N_("standard"), N_("user"), N_("custom"), N_("composite"), N_("bad adjust"), N_("by ROM"), N_("always"),
    N_("automatic, on"), N_("automatic"), N_("small"), N_("medium"), N_("large"), N_("gamepad"),
    N_("trak-ball"), N_("Amiga mouse"), N_("Atari mouse"), N_("light gun"), N_("Genesis pad")};


void App::emulatorOptionsDialog() {
  if (showEmuOptions_) {
    ImGui::OpenPopup("###emuopts");
    showEmuOptions_ = false;
    emuOptionsTab_ = project_ ? project_->emulator : "";
  }
  char title[96];
  snprintf(title, sizeof title, "%s###emuopts", T("Emulator options"));
  float fontSize = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(fontSize * 50, fontSize * 42), ImGuiCond_Appearing);
  bool open = true;
  if (!theme::beginModal(title, &open)) return;
  if (!project_) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  bool changed = false;
  if (ImGui::BeginTabBar("emus")) {
    for (const char* id : {"gopher2600", "stella"}) {
      const EmulatorInfo* emulator = kb_.emulator(id);
      if (!emulator) continue;
      ImGuiTabItemFlags tabFlags = emuOptionsTab_ == id ? ImGuiTabItemFlags_SetSelected : 0;
      if (!ImGui::BeginTabItem(emulator->name.c_str(), nullptr, tabFlags)) continue;
      auto& currentValues = project_->emulatorOptions[id];
      ImGui::BeginChild("opts", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 6), ImGuiChildFlags_Borders);
      // groups in the order they first appear, each with all its options
      std::vector<const EmuOption*> ordered;
      for (auto& groupOption : emulator->options)
        if (std::none_of(ordered.begin(), ordered.end(), [&](const EmuOption* candidate) { return candidate->group == groupOption.group; }))
          for (auto& option : emulator->options)
            if (option.group == groupOption.group) ordered.push_back(&option);
      std::string group;
      bool table = false;
      for (const EmuOption* optionPointer : ordered) {
        const EmuOption& option = *optionPointer;
        if (option.group != group || !table) {
          if (table) ImGui::EndTable();
          group = option.group;
          ImGui::SeparatorText(T(group.c_str()));
          table = ImGui::BeginTable(group.c_str(), 2, ImGuiTableFlags_SizingStretchProp);
          if (!table) continue;
          ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 3);
          ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 2);
        }
        ImGui::PushID(option.id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        auto current = currentValues.find(option.id);
        std::string value = current == currentValues.end() ? "" : current->second;
        if (value.empty()) ImGui::TextUnformatted(option.labelText().c_str());
        else ImGui::TextColored(theme::colv(theme::Role::MsgInfo), "%s", option.labelText().c_str());
        std::string tip = option.descriptionText() + "\n\n";
        if (!option.arg.empty()) tip += option.arg + (option.noValue ? "" : " <" + option.type + ">");
        else if (!option.flag.empty()) tip += option.flag;
        else tip += F("Preference {0} (VCSIDE passes it through a copy of the preferences file)", {option.pref});
        if (option.developer) tip += "\n" + std::string(T("Needs Stella's developer settings: VCSIDE turns them on."));
        wrappedTooltip(tip);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        std::string next = value;
        if (option.type == "int" || option.type == "text") {
          std::string hint = T("default");
          if (option.max > 0) hint += " (" + std::to_string(option.min) + "-" + std::to_string(option.max) + ")";
          ImGuiInputTextFlags flags = option.type == "int" ? ImGuiInputTextFlags_CharsDecimal : 0;
          ImGui::InputTextWithHint("##v", hint.c_str(), &next, flags);
          if (option.type == "int" && !next.empty() && option.max > 0) {
            int number = std::atoi(next.c_str());
            if (number < option.min || number > option.max) ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", T("out of range"));
          }
        } else {
          std::string preview = T("default");
          for (auto& choice : option.values)
            if (choice.value == value) preview = T(choice.label.c_str());
          if (ImGui::BeginCombo("##v", preview.c_str())) {
            if (ImGui::Selectable(T("default"), value.empty())) next.clear();
            for (auto& choice : option.values)
              if (ImGui::Selectable(T(choice.label.c_str()), choice.value == value)) next = choice.value;
            ImGui::EndCombo();
          }
        }
        wrappedTooltip(tip);
        if (next != value) {
          if (next.empty()) currentValues.erase(option.id);
          else currentValues[option.id] = next;
          changed = true;
        }
        ImGui::PopID();
      }
      if (table) ImGui::EndTable();
      ImGui::EndChild();

      std::string& extra = project_->emulatorArgs[id];
      ImGui::SetNextItemWidth(-fontSize * 12);
      if (ImGui::InputText(T("Extra arguments"), &extra)) changed = true;
      std::map<std::string, std::string> preferences;
      auto args = emulatorOptionArgs(*project_, *emulator, &preferences);
      for (auto& argument : splitCommand(extra)) args.push_back(argument);
      std::string line = args.empty() ? T("(nothing)") : quoteArgs(args);
      ImGui::TextDisabled("%s", T("Added to the command line:"));
      ImGui::SameLine();
      ImGui::TextWrapped("%s", line.c_str());
      if (!preferences.empty()) {
        std::string preferencesText;
        for (auto& [key, value] : preferences) preferencesText += (preferencesText.empty() ? "" : ", ") + key + " = " + value;
        ImGui::TextDisabled("%s", T("Preferences:"));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", preferencesText.c_str());
      }
      if (theme::dialogButton(T("Reset to defaults"))) {
        currentValues.clear();
        extra.clear();
        changed = true;
      }
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  emuOptionsTab_.clear();
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("%s", T("The emulators start with a copy of your own settings, so these options never change them; what you change in an emulator started from VCSIDE is not kept."));
  ImGui::PopTextWrapPos();
  if (theme::primaryButton(T("Close")) || theme::dialogEscape()) ImGui::CloseCurrentPopup();
  if (changed && !projectUnsaved_) {
    std::string error;
    if (!project_->save(&error)) lastStatus_ = error;
  }
  ImGui::EndPopup();
}

void App::goToAddressDialog() {
  if (showGoToAddress_) {
    ImGui::OpenPopup(T("Go to address"));
    showGoToAddress_ = false;
    snprintf(goToAddress_, sizeof goToAddress_, "%s", pendingAddress_.c_str());
    pendingAddress_.clear();
  }
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26, 0), ImGuiCond_Appearing);
  if (!theme::beginModal(T("Go to address"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextWrapped("%s", T("Address the CPU sees ($F1A0, F1A0 or a label), optionally followed by the bank: $F1A0 1"));
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  bool enter = ImGui::InputText("##addr", goToAddress_, sizeof goToAddress_, ImGuiInputTextFlags_EnterReturnsTrue);
  // parse "addr [bank]"
  std::istringstream stream(goToAddress_);
  std::string addressText, bankText;
  stream >> addressText >> bankText;
  std::optional<int> address;
  if (!addressText.empty()) {
    std::string expression = addressText;
    if (expression.size() > 2 && (expression[0] == '0') && (expression[1] == 'x' || expression[1] == 'X')) expression = "$" + expression.substr(2);
    else if (expression[0] != '$' && std::all_of(expression.begin(), expression.end(), [](char character) { return std::isxdigit((unsigned char)character); })) expression = "$" + expression;
    address = evalExpr(expression, [this](const std::string& name) { return lookupSymbol(name); });
    if (!address) address = evalExpr(addressText, [this](const std::string& name) { return lookupSymbol(name); });
  }
  int bank = bankText.empty() ? -1 : std::atoi(bankText.c_str());
  std::vector<const ListedLine*> hits;
  if (address) hits = addresses_.linesAt(*address, bank);
  bool shouldGo = false;
  const ListedLine* target = nullptr;
  if (!addressText.empty() && hits.empty()) ImGui::TextDisabled("%s", T("No code at that address in the last build."));
  for (size_t i = 0; i < hits.size() && i < 12; i++) {
    std::string label = fs::path(hits[i]->file).filename().string() + ":" + std::to_string(hits[i]->line);
    if (addresses_.banks() > 1) label += "  " + F("(bank {0})", {std::to_string(addresses_.bankOf(*hits[i]))});
    if (ImGui::Selectable(label.c_str(), i == 0)) target = hits[i], shouldGo = true;
  }
  if (enter && !hits.empty()) target = hits[0], shouldGo = true;
  if (shouldGo && target) {
    openFile(target->file, target->line);
    ImGui::CloseCurrentPopup();
  }
  if (theme::dialogEscape() || theme::dialogButton(T("Cancel"))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

static void spaceBar(const SpaceReport::Area& area, int warnPercent) {
  ImGui::TextUnformatted(area.name.c_str());
  ImGui::SameLine(ImGui::GetFontSize() * 9);
  if (!area.measured) {
    ImGui::TextDisabled("%s", area.note.c_str());
    return;
  }
  float frac = area.size > 0 ? (float)area.used / (float)area.size : 0;
  int percent = area.size > 0 ? (int)((area.used * 100 + area.size - 1) / area.size) : 0;
  ImVec4 colour = theme::colv(area.used > area.size ? theme::Role::SpaceFull : percent >= warnPercent ? theme::Role::SpaceWarning : theme::Role::SpaceOk);
  std::string label = F("{0} / {1} bytes ({2} %), {3} free",
                        {std::to_string(area.used), std::to_string(area.size), std::to_string(percent), std::to_string(std::max(0L, area.size - area.used))});
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colour);
  ImGui::ProgressBar(std::min(frac, 1.0f), ImVec2(ImGui::GetFontSize() * 24, 0), label.c_str());
  ImGui::PopStyleColor();
}

void App::spacePanel() {
  char title[64];
  snprintf(title, sizeof title, "%s###space", T("Space"));
  if (bottomDock_) ImGui::SetNextWindowDockID(bottomDock_, ImGuiCond_FirstUseEver);
  if (theme::beginPanel(title)) {
    if (spaceRunning_) ImGui::TextDisabled("%s", T("Measuring..."));
    if (!spaceNote_.empty()) ImGui::TextWrapped("%s", spaceNote_.c_str());
    if (elfSpace_) {
      int warnPercent = project_ ? project_->spaceWarnPercent : 90;
      auto& elfReport = *elfSpace_;
      ImGui::SeparatorText(F("ROM ({0} bytes)", {std::to_string(elfReport.fileSize)}).c_str());
      bool ace = elfReport.cartridge == ElfCartridge::Ace;
      // ACE: the header and start code, which stay in the file; ELF: the code that runs in RAM
      std::pair<const char*, long> parts[] = {{N_("Code"), elfReport.code}, {N_("Code in RAM (.RamFunc)"), elfReport.ramCode},
                                              {N_("Constants"), elfReport.constants}, {N_("Variables with a value"), elfReport.data},
                                              {N_("Zeroed variables"), elfReport.bss}};
      if (ace) parts[0] = {N_("Header and start code"), elfReport.startCode}, parts[1] = {N_("Code"), elfReport.code};
      if (ImGui::BeginTable("elfparts", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (auto& [name, byteCount] : parts) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(T(name));
          ImGui::TableNextColumn();
          ImGui::Text("%s", F("{0} bytes", {std::to_string(byteCount)}).c_str());
        }
        ImGui::EndTable();
      }
      auto bar = [&](const ElfSpace::Area& area) {
        SpaceReport::Area shown{area.name, area.used, area.size, true, ""};
        spaceBar(shown, warnPercent);
      };
      if (ace) {
        ImGui::SeparatorText(T("PlusCart / UnoCart"));
        bar(elfReport.flash);
        bar(elfReport.ram);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", T("The firmware copies the file to flash (up to 896 KB; on the PlusCart, the ROMs stored offline take part of it). The template's start code copies the code, the constants and the variables to SRAM, into the 96 KB the firmware used as its download buffer, after its arguments."));
        ImGui::SeparatorText(T("Gopher2600"));
        bar(elfReport.emulatorRam);
        ImGui::TextDisabled("%s", T("Gopher2600 gives an ACE file 128,000 bytes of SRAM, or 256,000 or 512,000 as the file grows, but only 64,000 from 448,000 bytes of file up."));
      } else if (elfReport.cartridge == ElfCartridge::PlusCart) {
        ImGui::SeparatorText(T("PlusCart / UnoCart"));
        bar(elfReport.ram);
        bar(elfReport.ccm);
        ImGui::PushTextWrapPos(0);
        if (elfReport.wholeFile)
          ImGui::TextDisabled("%s", T("The file fits the loader's buffer, so it is read whole into RAM. The zeroed variables go to the CCM RAM."));
        else
          ImGui::TextDisabled("%s", F("The file is bigger than the loader's buffer: it goes to flash, and the code, the variables with a value and the constants with pointers ({0} bytes) are copied to RAM. The zeroed variables go to the CCM RAM.",
                                      {std::to_string(elfReport.relocatedConstants)})
                                        .c_str());
        ImGui::TextDisabled("%s", T("Firmware 2.3.32 or later; with older ones the whole file must fit the 96 KB buffer."));
      } else {
        ImGui::SeparatorText(T("Chameleon Cart"));
        bar(elfReport.flash);
        bar(elfReport.ram);
        ImGui::PushTextWrapPos(0);
        if (elfReport.exact)
          ImGui::TextDisabled("%s", T("Measured on the Chameleon image (make chameleon), firmware included."));
        else
          ImGui::TextDisabled("%s", F("Estimate: the game plus about {0} bytes of flash and {1} of RAM for the firmware. Build the Chameleon image (make chameleon) for the exact figures.",
                                      {std::to_string(kChameleonFirmwareFlash), std::to_string(kChameleonFirmwareRam)})
                                        .c_str());
        ImGui::TextDisabled("%s", T("The 512 KB Chameleon Cart (STM32G0B0RE) has 512 KB of flash and 144 KB of RAM: set them in the project settings."));
      }
      if (!ace) ImGui::TextDisabled("%s", T("Stella and Gopher2600 load the whole ROM into RAM."));
      ImGui::PopTextWrapPos();
    } else if (!space_) {
      if (!spaceRunning_ && spaceNote_.empty()) ImGui::TextDisabled("%s", T("Build the project (F7) to see the space used."));
    } else {
      int warnPercent = project_ ? project_->spaceWarnPercent : 90;
      ImGui::SeparatorText(F("ROM ({0} bytes)", {std::to_string(space_->romFileSize)}).c_str());
      for (auto& bank : space_->banks) spaceBar(bank, warnPercent);
      ImGui::SeparatorText(T("RAM"));
      spaceBar(space_->ram, warnPercent);
      if (space_->ram.measured)
        ImGui::TextDisabled("%s", F("{0} bytes above the variables for the stack (reserve: {1}).",
                                     {std::to_string(space_->stackRoom), std::to_string(project_ ? project_->stackReserve : 8)})
                                      .c_str());
      if (space_->cartRam) spaceBar(*space_->cartRam, warnPercent);
    }
  }
  theme::endPanel();
}

// Shows whether an entry is fully verified and where its facts come from.
static void unverifiedNotice(bool verified) {
  if (verified) return;
  ImGui::PushStyleColor(ImGuiCol_Text, theme::colv(theme::Role::MsgWarning));
  ImGui::TextWrapped("%s", T("Contains statements not confirmed by the sources (marked \"Unverified\")."));
  ImGui::PopStyleColor();
}

static void provenance(const KnowledgeBase& kb, const std::vector<std::string>& sources) {
  if (sources.empty()) return;
  ImGui::SeparatorText(T("Sources"));
  for (auto& id : sources) {
    const Source* source = kb.source(id);
    ImGui::Bullet();
    if (!source) {
      ImGui::TextDisabled("%s", id.c_str());
      continue;
    }
    std::string label = source->title + (source->author.empty() ? "" : " (" + source->author + ")");
    if (source->url.rfind("http", 0) == 0) ImGui::TextLinkOpenURL(label.c_str(), source->url.c_str());
    else ImGui::TextWrapped("%s", label.c_str());
  }
}

static void bullet(const std::string& text) {
  ImGui::Bullet();
  ImGui::TextWrapped("%s", text.c_str());
}

void App::docsPanel() {
  char title[64];
  snprintf(title, sizeof title, "%s###docs", T("Documentation"));
  if (focusDocs_) showDocs_ = true, ImGui::SetNextWindowFocus();
  if (!showDocs_) return;
  if (!theme::beginPanel(title, &showDocs_)) {
    theme::endPanel();
    return;
  }
  if (focusDocs_ && !docTopic_.empty()) docQuery_ = docTopic_.find(':') == std::string::npos ? docTopic_ : std::string();  // a topic id ("K:...") is opened, not searched
  focusDocs_ = false;
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##q", T("Search"), &docQuery_);
  std::string query = upper(docQuery_);
  auto match = [&](const std::string& text) { return query.empty() || upper(text).find(query) != std::string::npos; };

  ImGui::BeginChild("list", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.35f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
  auto item = [&](const std::string& id, const std::string& label, ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (ImGui::Selectable((label + "##" + id).c_str(), docTopic_ == id)) docTopic_ = id;
    ImGui::PopStyleColor();
  };
  const ImVec4 legal = theme::colv(theme::Role::SynMnemonic), illegal = theme::colv(theme::Role::SynIllegal),
               unstable = theme::colv(theme::Role::SynUnstable), plain = theme::colv(theme::Role::UiText);
  // the root: clicking it goes back to the general notes
  ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
  if (docTopic_.empty()) rootFlags |= ImGuiTreeNodeFlags_Selected;
  bool rootOpen = ImGui::TreeNodeEx("VCS##docroot", rootFlags);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) docTopic_.clear(), docQuery_.clear();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The Atari 2600: general notes"));
  if (rootOpen) {
  // A branch that leads to nothing is not drawn; while searching, the ones that do lead somewhere are open.
  auto openBranch = [&](const char* label, bool firstTime) {
    if (!query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    else if (firstTime) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    return ImGui::TreeNode(T(label));
  };
  const char* groups[] = {N_("Legal"), N_("Illegal (stable)"), N_("Illegal (unstable)")};
  std::vector<std::vector<const Instruction*>> groupItems(3);
  for (auto& [mnemonic, instruction] : kb_.instructions()) {
    bool stable = instruction.opcodes.empty() || instruction.opcodes[0]->stable;
    if (match(mnemonic) || match(instruction.summary)) groupItems[!instruction.illegal ? 0 : stable ? 1 : 2].push_back(&instruction);
  }
  if (!groupItems[0].empty() || !groupItems[1].empty() || !groupItems[2].empty()) {
    if (openBranch("Instructions", true)) {
      for (int groupIndex = 0; groupIndex < 3; groupIndex++) {
        if (groupItems[groupIndex].empty()) continue;
        if (!query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (ImGui::TreeNodeEx(T(groups[groupIndex]), groupIndex == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
          for (auto* instruction : groupItems[groupIndex])
            item("I:" + instruction->mnemonic, instruction->mnemonic + "  " + instruction->summary, groupIndex == 0 ? legal : groupIndex == 1 ? illegal : unstable);
          ImGui::TreePop();
        }
      }
      ImGui::TreePop();
    }
  }
  std::vector<const Register*> shownRegisters;
  for (auto& registerEntry : kb_.registers())
    if (match(registerEntry.name) || match(registerEntry.summary)) shownRegisters.push_back(&registerEntry);
  if (!shownRegisters.empty() && openBranch("Registers", false)) {
    for (auto* registerEntry : shownRegisters) item("R:" + registerEntry->name, registerEntry->name + "  " + registerEntry->address + "  " + registerEntry->summary, plain);
    ImGui::TreePop();
  }
  std::vector<const Technique*> shownRecipes;
  for (auto& technique : kb_.techniques()) {
    bool keyword = false;
    for (auto& word : technique.keywords) keyword = keyword || match(word);
    if (match(technique.title) || match(technique.summary) || keyword) shownRecipes.push_back(&technique);
  }
  if (!shownRecipes.empty() && openBranch("Recipes", false)) {
    for (auto* technique : shownRecipes) item("K:" + technique->id, technique->title, plain);
    ImGui::TreePop();
  }
  std::vector<std::pair<std::string, std::vector<const CartFormat*>>> shownFormats;
  for (auto& [category, categoryTitle] : formatCategories()) {
    std::vector<const CartFormat*> categoryFormats;
    for (auto& cartFormat : kb_.formats())
      if (cartFormat.category == category && (match(cartFormat.id) || match(cartFormat.name))) categoryFormats.push_back(&cartFormat);
    if (categoryFormats.empty()) continue;
    std::sort(categoryFormats.begin(), categoryFormats.end(), [](auto* left, auto* right) { return naturalLess(left->name, right->name); });
    shownFormats.push_back({categoryTitle, categoryFormats});
  }
  if (!shownFormats.empty() && openBranch("Cartridge formats", false)) {
    for (auto& [categoryTitle, categoryFormats] : shownFormats) {
      ImGui::SeparatorText(categoryTitle.c_str());
      for (auto* cartFormat : categoryFormats) item("F:" + cartFormat->id, cartFormat->id + "  " + cartFormat->name, plain);
    }
    ImGui::TreePop();
  }
  std::vector<const TvStandard*> shownTv;
  for (auto& tvStandard : kb_.tvStandards())
    if (match(tvStandard.id)) shownTv.push_back(&tvStandard);
  if (!shownTv.empty() && openBranch("TV standards", false)) {
    for (auto* tvStandard : shownTv) item("T:" + tvStandard->id, tvStandard->id, plain);
    ImGui::TreePop();
  }
  ImGui::TreePop();
  }
  ImGui::EndChild();

  ImGui::BeginChild("topic");
  // registers and instructions named in the cards open their own card
  assistant::MdLinks docLinks{&kb_, [this](const std::string& name) { docTopic_ = name; }};
  assistant::setMarkdownLinks(&docLinks);
  auto seeAlso = [&](const std::vector<DocLink>& links) {
    if (links.empty()) return;
    ImGui::SeparatorText(T("See also"));
    for (auto& link : links)
      if (ImGui::Selectable((link.label + "##see" + link.topic).c_str(), false)) docTopic_ = link.topic;
  };
  std::string topic = docTopic_;
  // a bare word (from F1) resolves to an instruction or a register
  if (topic.find(':') == std::string::npos && !topic.empty()) {
    if (kb_.instruction(topic)) topic = "I:" + upper(topic);
    else if (kb_.reg(topic)) topic = "R:" + kb_.reg(topic)->name;
    else if (kb_.format(topic)) topic = "F:" + kb_.format(topic)->id;
    docTopic_ = topic;
  }
  if (topic.rfind("I:", 0) == 0) {
    if (const Instruction* instruction = kb_.instruction(topic.substr(2))) {
      bool stable = instruction->opcodes.empty() || instruction->opcodes[0]->stable;
      ImGui::PushFont(mono_);
      ImGui::TextColored(!instruction->illegal ? legal : stable ? illegal : unstable, "%s", instruction->mnemonic.c_str());
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::TextUnformatted(instruction->summary.c_str());
      if (instruction->illegal) ImGui::TextColored(stable ? illegal : unstable, "%s", stable ? T("Illegal (stable)") : T("Illegal (unstable)"));
      if (!instruction->flags.empty()) ImGui::Text("%s: %s", T("Flags"), instruction->flags.c_str());
      unverifiedNotice(instruction->verified);
      if (!instruction->aliases.empty()) {
        std::string aliasText;
        for (auto& alias : instruction->aliases) aliasText += (aliasText.empty() ? "" : ", ") + alias;
        ImGui::TextDisabled("%s", F("Also known as: {0}", {aliasText}).c_str());
      }
      ImGui::TextWrapped("%s", instruction->description.c_str());
      if (ImGui::BeginTable("ops", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(T("Mode"));
        ImGui::TableSetupColumn(T("Opcode"));
        ImGui::TableSetupColumn(T("Bytes"));
        ImGui::TableSetupColumn(T("Cycles"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (auto* opcode : instruction->opcodes) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::PushFont(mono_);
          std::string assembly = instruction->mnemonic;
          auto& modes = opcode->mode;
          static const std::map<std::string, std::string> syntax = {
              {"imp", ""}, {"acc", " A"}, {"imm", " #$nn"}, {"zp", " $nn"}, {"zpx", " $nn,X"}, {"zpy", " $nn,Y"},
              {"abs", " $nnnn"}, {"absx", " $nnnn,X"}, {"absy", " $nnnn,Y"}, {"ind", " ($nnnn)"},
              {"indx", " ($nn,X)"}, {"indy", " ($nn),Y"}, {"rel", " label"}};
          auto found = syntax.find(modes);
          ImGui::TextUnformatted((assembly + (found != syntax.end() ? found->second : "")).c_str());
          ImGui::TableNextColumn();
          ImGui::Text("$%02X", opcode->code);
          ImGui::TableNextColumn();
          ImGui::Text("%d", opcode->bytes);
          ImGui::TableNextColumn();
          if (opcode->branch) ImGui::Text("%d (+1/+2)", opcode->cycles);
          else if (opcode->pagePenalty) ImGui::Text("%d (+1)", opcode->cycles);
          else ImGui::Text("%d", opcode->cycles);
          ImGui::PopFont();
          ImGui::TableNextColumn();
          if (opcode->illegal) ImGui::TextColored(opcode->stable ? illegal : unstable, "%s", opcode->stable ? T("illegal") : T("unstable"));
          if (!opcode->dasm) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T(".byte only"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("DASM has no mnemonic for this opcode: write it with .byte"));
          }
        }
        ImGui::EndTable();
      }
      ImGui::TextDisabled("%s", T("(+1) page crossing; branches: +1 taken, +2 taken to another page"));
      provenance(kb_, instruction->sources);
    }
  } else if (topic.rfind("R:", 0) == 0) {
    if (const Register* reg = kb_.reg(topic.substr(2))) {
      ImGui::PushFont(mono_);
      ImGui::TextColored(theme::colv(theme::Role::MsgName), "%s", reg->name.c_str());
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::Text("%s %s  %s  bits %s", reg->chip.c_str(), reg->address.c_str(), reg->access == "R" ? T("read") : T("write"), reg->bits.c_str());
      ImGui::TextWrapped("%s", reg->summary.c_str());
      unverifiedNotice(reg->verified);
      ImGui::Separator();
      assistant::drawMarkdownWithCode(registerMarkdown(*reg), mono_);
      seeAlso(registerSeeAlso(kb_, *reg));
      provenance(kb_, reg->sources);
    }
  } else if (topic.rfind("K:", 0) == 0) {
    const Technique* recipe = nullptr;
    for (auto& technique : kb_.techniques())
      if (technique.id == topic.substr(2)) recipe = &technique;
    if (recipe) {
      ImGui::PushFont(mono_);
      ImGui::TextColored(theme::colv(theme::Role::MsgName), "%s", recipe->title.c_str());
      ImGui::PopFont();
      unverifiedNotice(recipe->verified);
      ImGui::Separator();
      assistant::drawMarkdownWithCode(techniqueMarkdown(*recipe), mono_);
      seeAlso(techniqueSeeAlso(kb_, *recipe));
      provenance(kb_, recipe->sources);
    }
  } else if (topic.rfind("F:", 0) == 0) {
    if (const CartFormat* cartFormat = kb_.format(topic.substr(2))) {
      ImGui::Text("%s - %s", cartFormat->id.c_str(), cartFormat->name.c_str());
      unverifiedNotice(cartFormat->verified);
      ImGui::Text("%s: %s", T("Size"), cartFormat->size.c_str());
      if (!cartFormat->ram.empty()) ImGui::TextWrapped("RAM: %s", cartFormat->ram.c_str());
      ImGui::TextWrapped("%s", cartFormat->description.c_str());
      if (!cartFormat->hotspots.empty()) {
        ImGui::SeparatorText(T("Hotspots"));
        for (auto& hotspot : cartFormat->hotspots) bullet(hotspot);
      }
      if (!cartFormat->detection.empty()) {
        ImGui::SeparatorText(T("Detection"));
        ImGui::TextWrapped("%s", cartFormat->detection.c_str());
      }
      ImGui::SeparatorText(T("Emulator"));
      if (cartFormat->stella == "none") ImGui::Text("Stella  %s", T("does not run it"));
      else ImGui::Text("Stella  -bs %s", cartFormat->stella.empty() ? "?" : cartFormat->stella.c_str());
      ImGui::Text("Gopher2600 -mapping %s", cartFormat->gopher2600.empty() ? "?" : cartFormat->gopher2600.c_str());
      if (!cartFormat->issues.empty()) {
        ImGui::SeparatorText(T("Known issues"));
        for (auto& issue : cartFormat->issues) bullet(issue);
      }
      provenance(kb_, cartFormat->sources);
    }
  } else if (topic.rfind("T:", 0) == 0) {
    if (const TvStandard* tvStandard = kb_.tv(topic.substr(2))) {
      ImGui::Text("%s", tvStandard->id.c_str());
      ImGui::TextUnformatted(F("{0} lines: VSYNC {1}, VBLANK {2}, visible {3}, overscan {4}",
                               {std::to_string(tvStandard->linesTotal), std::to_string(tvStandard->vsync), std::to_string(tvStandard->vblank),
                                std::to_string(tvStandard->visible), std::to_string(tvStandard->overscan)}).c_str());
      ImGui::Text("%s Hz", tvStandard->fps.c_str());
      unverifiedNotice(tvStandard->verified);
      ImGui::TextWrapped("%s", tvStandard->palette.c_str());
      for (auto& note : tvStandard->notes) bullet(note);
      provenance(kb_, tvStandard->sources);
    }
  } else {
    ImGui::PushFont(mono_);
    ImGui::TextColored(theme::colv(theme::Role::MsgName), "VCS");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextUnformatted(T("The Atari 2600: general notes"));
    ImGui::SeparatorText(T("6507 CPU"));
    for (auto& note : kb_.cpuNotes()) bullet(note);
    ImGui::SeparatorText("TIA");
    for (auto& note : kb_.tiaNotes()) bullet(note);
  }
  assistant::setMarkdownLinks(nullptr);
  ImGui::EndChild();
  theme::endPanel();
}

void App::newProjectDialog() {
  if (showNewProject_) {
    ImGui::OpenPopup("###newproject");
    showNewProject_ = false;
    if (npDir_.empty()) npDir_ = projectsDir_;  // where the last project was made
    std::error_code folderError;
    if (npDir_.empty() || !fs::is_directory(npDir_, folderError)) {
      const char* home = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
      npDir_ = home ? home : "";
    }
  }
  char title[64];
  snprintf(title, sizeof title, "%s###newproject", T("New project..."));
  const Language npLanguage = languageFromId(kLangIds[npLang_]);
  // DASM and batari Basic: the cartridge, and the templates made for it ("" is the empty one)
  const auto formats = Project::cartridgeFormats(npLanguage);
  if (!formats.empty() && std::find(formats.begin(), formats.end(), npFormat_) == formats.end()) npFormat_ = "4K";
  auto examples = Project::examples(npLanguage, formats.empty() ? "" : npFormat_);
  std::vector<std::string> startIds;
  std::vector<const char*> startNames;
  if (Project::hasEmptyTemplate(npLanguage, npFormat_) || formats.empty()) startIds.push_back(""), startNames.push_back(T("Empty project"));
  for (auto& exampleEntry : examples) startIds.push_back(exampleEntry.first), startNames.push_back(T(exampleEntry.second));
  auto formatLabel = [](const std::string& format) {  // the size, for the banked ones
    for (auto [id, size] : {std::pair{"F8", "8K"}, {"F6", "16K"}, {"F4", "32K"}, {"DPC+", "32K"}, {"CDFJ", "32K"}})
      if (format == id) return format + " (" + size + ")";
    return format;
  };
  {
    // wide enough for its longest label and value (the folder grows as it is typed): never squeezed
    const ImGuiStyle& style = ImGui::GetStyle();
    float labels = 0, values = 0;
    for (const char* label : {"Name", "Folder", "Programming language", "TV standard", "Start from", "Cartridge", "Flash (KB)", "RAM (KB)"})
      labels = std::max(labels, ImGui::CalcTextSize(T(label)).x);
    auto value = [&](const std::string& text) { values = std::max(values, ImGui::CalcTextSize(text.c_str()).x); };
    for (const char* name : kLangNames) value(T(name));
    for (auto& tv : tvStandardsFor(languageFromId(kLangIds[npLang_]))) value(tvLabel(tv) + "  " + F("(not in {0})", {"Gopher2600"}));
    value(T("Empty project"));
    for (auto& exampleEntry : Project::examples(npLanguage)) value(T(exampleEntry.second));
    for (auto& format : formats) value(formatLabel(format));
    float chooseWidth = ImGui::CalcTextSize(T("Choose...")).x + theme::em(14) * 2 + style.ItemSpacing.x;
    float folder = ImGui::CalcTextSize(npDir_.c_str()).x + style.FramePadding.x * 2 + chooseWidth;
    float fields = std::max(values + style.FramePadding.x * 2 + ImGui::GetFrameHeight(), folder);
    float target = ImGui::CalcTextSize((fs::path(npDir_) / npName_).string().c_str()).x;
    float width = std::max(labels + style.CellPadding.x * 2 + fields, target) + theme::em(16) * 2 + theme::em(8);
    width = std::min(std::max(width, theme::em(440)), ImGui::GetMainViewport()->WorkSize.x * 0.9f);
    // the height fits the rows every frame (a language adds or removes some): never a scrollbar
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
  }
  if (!theme::beginModal(title, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) return;
  float em = ImGui::GetFontSize() / 15.0f;
  auto row = [&](const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
  };
  if (ImGui::BeginTable("##np", 2, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
    row(T("Name"));
    ImGui::InputText("##name", npName_, sizeof npName_);
    row(T("Folder"));
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(T("Choose...")).x + theme::em(14) * 2 + ImGui::GetStyle().ItemSpacing.x));  // dialogButton's padding
    ImGui::InputText("##folder", &npDir_);
    ImGui::SameLine();
    if (theme::dialogButton(T("Choose...")))
      showFolderDialog(dialogCallback, new DialogCtx{this, "newdir"}, window_, npDir_.c_str());
    const char* langNames[std::size(kLangNames)];
    for (size_t i = 0; i < std::size(kLangNames); i++) langNames[i] = T(kLangNames[i]);
    row(T("Programming language"));
    ImGui::Combo("##lang", &npLang_, langNames, (int)std::size(kLangNames));
    {
      // only the standards this language can build for; the choice stays if the new list has it
      auto ids = tvStandardsFor(languageFromId(kLangIds[npLang_]));
      ids.push_back("AUTO");
      if (std::find(ids.begin(), ids.end(), npTvId_) == ids.end()) npTvId_ = ids[0];
      row(T("TV standard"));
      if (ImGui::BeginCombo("##tv", tvLabel(npTvId_).c_str())) {
        for (auto& tv : ids) {
          if (tv == "AUTO") ImGui::Separator();
          bool selected = tv == npTvId_;
          std::string label = tvLabel(tv);
          if (tv != "AUTO" && !tvSupported("gopher2600", tv)) label += "  " + F("(not in {0})", {"Gopher2600"});  // the default emulator
          if (ImGui::Selectable(label.c_str(), selected)) npTvId_ = tv;
          if (ImGui::IsItemHovered()) {
            if (tv == "AUTO") ImGui::SetTooltip("%s", T(kTvMultipleHelp));
            else if (const TvStandard* tvStandard = kb_.tv(tv)) ImGui::SetTooltip("%s", F("{0} lines, {1} Hz", {std::to_string(tvStandard->linesTotal), tvStandard->fps}).c_str());
          }
        }
        ImGui::EndCombo();
      }
    }
    if (languageFromId(kLangIds[npLang_]) == Language::ElfC) {
      row(T("Cartridge"));
      const std::pair<const char*, const char*> cartridges[] = {{"chameleon", "Chameleon Cart"}, {"pluscart", N_("PlusCart or UnoCart")}};
      std::string shown = npElfCartridge_ == "pluscart" ? T(cartridges[1].second) : cartridges[0].second;
      if (ImGui::BeginCombo("##cart", shown.c_str())) {
        for (auto& [id, name] : cartridges)
          if (ImGui::Selectable(id == std::string("pluscart") ? T(name) : name, npElfCartridge_ == id) && npElfCartridge_ != id) {
            npElfCartridge_ = id;
            npFlashKb_ = (int)defaultElfFlashKb(id), npRamKb_ = (int)defaultElfRamKb(id);
          }
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("What the space report measures the game against. It can be changed later in the project settings."));
      if (npElfCartridge_ == "chameleon") {
        row(T("Flash (KB)"));
        ImGui::InputInt("##flash", &npFlashKb_, 0);
        npFlashKb_ = std::clamp(npFlashKb_, 1, 65536);
      }
      row(T("RAM (KB)"));
      ImGui::InputInt("##ram", &npRamKb_, 0);
      npRamKb_ = std::clamp(npRamKb_, 1, 65536);
    }
    if (!formats.empty()) {
      row(T("Cartridge"));
      if (ImGui::BeginCombo("##format", formatLabel(npFormat_).c_str())) {
        for (auto& format : formats)
          if (ImGui::Selectable(formatLabel(format).c_str(), format == npFormat_) && format != npFormat_) npFormat_ = format, npExample_ = 0;
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("The ROM's size and bank switching. The template's code is written for it: the project settings can change the format later, not the code."));
    }
    npExample_ = std::clamp(npExample_, 0, (int)startIds.size() - 1);
    if (startIds.size() > 1) {
      row(T("Start from"));
      ImGui::Combo("##example", &npExample_, startNames.data(), (int)startNames.size());
    }
    ImGui::EndTable();
  }
  // drivers VCSIDE cannot include: said before Create, not found out at the first build
  if (npLanguage == Language::Dasm && npFormat_ == "CDFJ") {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::colv(theme::Role::MsgWarning), "%s", T(kCdfjDriverHelp));
    ImGui::PopTextWrapPos();
    if (theme::dialogButton(T("Choose file...")))
      SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "npcdfj"}, window_, nullptr, 0, nullptr, false);
    if (!npCdfjDriver_.empty()) ImGui::SetItemTooltip("%s", npCdfjDriver_.c_str());
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(0);
    if (!npCdfjError_.empty()) ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", npCdfjError_.c_str());
    else if (!npCdfjDriver_.empty()) ImGui::TextUnformatted(F("Driver: {0}", {fs::path(npCdfjDriver_).filename().string()}).c_str());
    else ImGui::TextDisabled("%s", T("No driver chosen: the project is made without it."));
    ImGui::PopTextWrapPos();
  } else if (npLanguage == Language::Dasm && npFormat_ == "DPC+" && Project::dpcPlusDriver().empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::colv(theme::Role::MsgWarning), "%s",
                       T("VCSIDE does not include DPC+'s ARM driver (DPCplus.arm): the project takes a copy from batari Basic. Install batari Basic first (Tools)."));
    ImGui::PopTextWrapPos();
  }
  fs::path target = fs::path(npDir_) / npName_;
  ImGui::TextDisabled("%s", target.string().c_str());
  static std::string error;
  if (!error.empty()) ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", error.c_str());
  ImGui::Dummy(ImVec2(0, 6 * em));
  if (theme::primaryButton(T("Create"))) {
    Project created;
    error.clear();
    std::string example = startIds[npExample_];
    bool made = Project::create(target.string(), npName_, npLanguage, npTvId_, &created, &error, example, formats.empty() ? "" : npFormat_);
    if (made && created.language == Language::ElfC) {  // the cartridge; its own sizes are left unset
      created.elfCartridge = npElfCartridge_;
      created.elfFlashKb = npFlashKb_ == defaultElfFlashKb(npElfCartridge_) ? 0 : npFlashKb_;
      created.elfRamKb = npRamKb_ == defaultElfRamKb(npElfCartridge_) ? 0 : npRamKb_;
      made = created.save(&error);
    }
    if (made && npLanguage == Language::Dasm && npFormat_ == "CDFJ" && !npCdfjDriver_.empty()) {
      std::string driverError;
      if (!Project::addCdfjDriver(npCdfjDriver_, created.dir, &driverError)) output_ += driverError + "\n", outputScroll_ = true;
    }
    if (made) {
      npCdfjDriver_.clear(), npCdfjError_.clear();
      if (projectsDir_ != npDir_) projectsDir_ = npDir_, saveSettings();  // the next new project goes there too
      openProject(created.file);
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (theme::dialogButton(T("Cancel")) || theme::dialogEscape()) {
    error.clear();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

bool App::createProjectFile() {
  std::string error;
  if (!project_ || !project_->save(&error)) {
    output_ += error + "\n";
    lastStatus_ = error;
    outputScroll_ = true;
    return false;
  }
  std::string file = project_->file;
  openProject(file);  // as any project: symbols, recent projects, the window's title
  return true;
}

void App::noProjectDialog() {
  if (showNoProject_) ImGui::OpenPopup("###noproject"), showNoProject_ = false;
  char title[96];
  snprintf(title, sizeof title, "%s###noproject", T("No project file"));
  if (!theme::beginModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  if (!buildCandidate_) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const Project& candidate = *buildCandidate_;
  ImGui::TextUnformatted(F("{0} has no project file, but it can be built:", {fs::path(candidate.dir).filename().string()}).c_str());
  if (ImGui::BeginTable("##found", 2, ImGuiTableFlags_SizingFixedFit)) {
    auto row = [](const char* label, const std::string& value) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", label);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(value.c_str());
    };
    row(T("Programming language"), languageName(candidate.language));
    row(T("Main source"), candidate.main);
    row(T("Builds with"), !candidate.buildCommand.empty() ? candidate.buildCommand
                          : candidate.language == Language::BatariBasic ? std::string("batari Basic") : std::string("DASM"));
    row(T("ROM"), candidate.output);
    ImGui::EndTable();
  }
  ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f));
  enum class Choice { None, Build, CreateAndBuild, CreateAndConfigure, Cancel } choice = Choice::None;
  if (theme::primaryButton(T("Build"))) choice = Choice::Build;
  ImGui::SameLine();
  if (theme::dialogButton(T("Create project and build"))) choice = Choice::CreateAndBuild;
  ImGui::SameLine();
  if (theme::dialogButton(T("Create and configure..."))) choice = Choice::CreateAndConfigure;
  ImGui::SameLine();
  if (theme::dialogButton(T("Cancel")) || theme::dialogEscape()) choice = Choice::Cancel;
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
  ImGui::TextDisabled("%s", F("Build keeps these settings until the folder is closed, and writes nothing. A project file ({0}) keeps them for good, and Build > Project settings... changes them.",
                              {fs::path(candidate.file).filename().string()}).c_str());
  ImGui::PopTextWrapPos();
  if (choice != Choice::None) {
    ImGui::CloseCurrentPopup();
    if (choice != Choice::Cancel) {
      project_ = candidate;
      projectUnsaved_ = true;
      bool ready = choice == Choice::Build || createProjectFile();
      if (ready && choice == Choice::CreateAndConfigure) projectSettings();
      else if (ready) build(candidateRun_);
    }
    buildCandidate_.reset();
  }
  ImGui::EndPopup();
}

void App::toolsDialog() {
  if (showTools_) {
    ImGui::OpenPopup("###tools");
    showTools_ = false;
  }
  char title[64];
  snprintf(title, sizeof title, "%s###tools", T("Tools"));
  ImGui::SetNextWindowSize(ImVec2(std::min(ImGui::GetFontSize() * 58, ImGui::GetMainViewport()->WorkSize.x - 40), 0), ImGuiCond_Appearing);
  bool open = true;
  if (!theme::beginModal(title, &open)) return;
  float em = ImGui::GetFontSize() / 15.0f;
  for (auto& group : Toolchain::groups()) {
    auto list = tools_.inGroup(group);
    if (list.empty()) continue;
    theme::dialogHeading(T(group.title));
    if (!ImGui::BeginTable(group.title, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) continue;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 180 * em);
    ImGui::TableSetupColumn("path");
    ImGui::TableSetupColumn("buttons", ImGuiTableColumnFlags_WidthFixed, 230 * em);
    for (Tool* toolPointer : list) {
      Tool& tool = *toolPointer;
      ImGui::PushID(tool.id.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(tool.name.c_str());
      ImGui::TextDisabled("%s", T(tool.role.c_str()));
      ImGui::TableNextColumn();
      if (tool.found()) {
        ImGui::TextColored(theme::colv(theme::Role::MsgOk), "%s", tool.path.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tool.path.c_str());  // in full, when the column cuts it
      }
      else {
        ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", T("not found"));
        ImGui::TextWrapped("%s", T(tool.note.c_str()));
      }
      ImGui::TableNextColumn();
      if (theme::dialogButton(T("Choose..."), ImVec2(0, 0), true))
        SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "tool:" + tool.id}, window_, nullptr, 0, nullptr, false);
      ImGui::SameLine();
      if (theme::dialogButton(T("Download page"), ImVec2(0, 0), true)) SDL_OpenURL(tool.homepage.c_str());
      if (!tool.userPath.empty()) {
        ImGui::SameLine();
        if (theme::dialogButton("x", ImVec2(0, 0), true)) tool.userPath.clear(), tools_.detect(), saveSettings();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Forget the chosen file and detect it again"));
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::Dummy(ImVec2(0, 6 * em));
  }
  ImGui::TextDisabled("%s", Toolchain::toolsDir().c_str());
  ImGui::Dummy(ImVec2(0, 4 * em));
  if (theme::primaryButton(T("Close")) || theme::dialogEscape()) ImGui::CloseCurrentPopup();
  ImGui::SameLine();
  if (theme::dialogButton(T("Detect again"))) tools_.detect();
  ImGui::EndPopup();
}

// A tool the project needs is missing: say what it is, how to get it, and let the user
// point at it; then carry on with what was asked (build or run).
void App::askForTool(const std::string& id, const std::string& why, std::function<void()> then) {
  Tool* tool = tools_.get(id);
  if (!tool) return;
  missingTool_ = id;
  missingWhy_ = why;
  missingThen_ = std::move(then);
  showMissingTool_ = true;
  std::string message = F("{0} not found: it is needed to {1}.", {tool->name, why});
  output_ += message + "\n";
  lastStatus_ = message;
  outputScroll_ = true;
}

void App::missingToolDialog() {
  if (showMissingTool_) {
    ImGui::OpenPopup("###missingtool");
    showMissingTool_ = false;
  }
  Tool* tool = tools_.get(missingTool_);
  if (!tool) return;
  char title[128];
  snprintf(title, sizeof title, "%s###missingtool", (tool->found() ? F("{0} found", {tool->name}) : F("{0} not found", {tool->name})).c_str());
  float em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(em * 36, 0), ImGuiCond_Appearing);
  if (!theme::beginModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::PushTextWrapPos(em * 34);
  if (!tool->found()) {
    ImGui::TextUnformatted(F("VCSIDE needs {0} ({1}) to {2}, and did not find it on this computer.", {tool->name, T(tool->role.c_str()), missingWhy_}).c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(T(tool->note.c_str()));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", F("Once installed, VCSIDE finds it in PATH, in the usual folders, or in {0}. You can also choose the file yourself.",
                                {Toolchain::toolsDir()}).c_str());
  } else {
    ImGui::TextColored(theme::colv(theme::Role::MsgOk), "%s", F("Found: {0}", {tool->path}).c_str());
  }
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  bool close = false;
  if (tool->found()) {
    if (theme::primaryButton(T("Continue"))) {
      close = true;
      if (missingThen_) missingThen_();
    }
  } else {
    if (theme::dialogButton(T("Open download page"))) SDL_OpenURL(tool->homepage.c_str());
    ImGui::SameLine();
    if (theme::dialogButton(T("Choose the file...")))
      SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "tool:" + tool->id}, window_, nullptr, 0, nullptr, false);
    ImGui::SameLine();
    if (theme::dialogButton(T("Search again"))) tools_.detect();
  }
  ImGui::SameLine();
  if (theme::dialogButton(T("Close")) || theme::dialogEscape()) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    missingThen_ = nullptr;
  }
  ImGui::EndPopup();
}

// The icon as an OpenGL texture, made the first time it is needed (0 if missing).
unsigned App::iconTexture() {
  if (!aboutTexture_) {
    if (SDL_Surface* icon = loadIconSurface()) {
      GLuint texture = 0;
      glGenTextures(1, &texture);
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, icon->pitch / 4);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, icon->w, icon->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon->pixels);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      aboutTexture_ = texture;
      SDL_DestroySurface(icon);
    }
  }
  return aboutTexture_;
}

void App::aboutDialog() {
  if (showAbout_) {
    showAbout_ = false;
    // macOS: the system's panel; elsewhere the window below
    if (nativeMenuAvailable() && nativeShowAbout(aboutInfo(kb_, tools_))) return;
    about_ = aboutInfo(kb_, tools_);
    ImGui::OpenPopup("###about");
  }
  if (about_) iconTexture();
  char title[64];
  snprintf(title, sizeof title, "%s###about", T("About VCSIDE"));
  float em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(em * 44, em * 38), ImGuiCond_Appearing);
  if (!theme::beginModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
  if (!about_) about_ = aboutInfo(kb_, tools_);
  const AboutInfo& info = *about_;
  if (aboutTexture_) {
    ImGui::Image((ImTextureID)(intptr_t)aboutTexture_, ImVec2(em * 6, em * 6));
    ImGui::SameLine(0, em);
  }
  ImGui::BeginGroup();
  ImGui::SetWindowFontScale(1.6f);
  ImGui::TextUnformatted("VCSIDE");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::TextUnformatted(F("Version {0}", {info.version}).c_str());
  ImGui::TextUnformatted(info.tagline.c_str());
  ImGui::TextDisabled("%s", info.credit.c_str());
  ImGui::TextDisabled("%s", info.copyright.c_str());
  ImGui::EndGroup();
  if (!kbError_.empty()) ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", kbError_.c_str());
  ImGui::Spacing();
  ImGui::BeginChild("credits", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f), ImGuiChildFlags_Borders);
  ImGui::PushTextWrapPos(0);
  for (auto& section : info.sections) {
    ImGui::SeparatorText(section.title.c_str());
    if (!section.note.empty()) ImGui::TextDisabled("%s", section.note.c_str());
    for (auto& line : section.lines) {
      ImGui::PushID(&line);
      if (!line.url.empty()) ImGui::TextLinkOpenURL(line.name.c_str(), line.url.c_str());
      else ImGui::TextUnformatted(line.name.c_str());
      if (!line.detail.empty()) {
        ImGui::Indent(em);
        ImGui::TextDisabled("%s", line.detail.c_str());
        ImGui::Unindent(em);
      }
      ImGui::PopID();
    }
  }
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
  ImGui::TextDisabled("%s", F("Data: {0}", {info.dataDir}).c_str());
  if (theme::primaryButton(T("Close")) || theme::dialogEscape()) {
    ImGui::CloseCurrentPopup();
    about_.reset();
  }
  ImGui::EndPopup();
}

}  // namespace vcs
