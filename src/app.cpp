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
#include "file_icons.h"
#include "cycles.h"
#include "window_icon.h"
#include "emulator_launch.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "paths.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace vcs {

static const int kLayoutVersion = 2;  // bump when a docked panel is added
static const char* kLangIds[] = {"dasm", "batari-basic", "elf-c"};
static const char* kLangNames[] = {N_("Assembler (DASM)"), N_("batari Basic"), N_("C for ELF (vcsLib)")};

static std::string readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static bool isAsmFile(const std::string& path) {
  std::string e = upper(fs::path(path).extension().string());
  return e == ".ASM" || e == ".S" || e == ".H" || e == ".INC" || e == ".A" || e == ".65S";
}

static std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// ---------------------------------------------------------------------------
// Setup

bool App::init(SDL_Window* window, ImFont* mono, bool imguiMenu) {
  window_ = window;
  mono_ = mono;
  if (!kb_.load(dataDir() + "/kb", &kbError_)) SDL_Log("Knowledge base: %s", kbError_.c_str());
  loadSettings();
  tools_.detect();
  if (nativeMenuAvailable() && !imguiMenu) {
    nativeMenu_ = true;
    nativeMenuInstall({[this] { return buildMenuBar(true); }, [this] { requestQuit(); }, [this] { showAbout_ = true; }});
  }
  return true;
}

void App::shutdown() { saveSettings(); }

void App::loadSettings() {
  json j;
  try {
    std::ifstream f(configDir() + "settings.json");
    if (f) j = json::parse(f);
  } catch (...) {
  }
  if (!j.is_object()) j = json::object();
  loadLanguages(dataDir() + "/i18n");
  std::string code = j.value("language", "");
  setLanguage(code.empty() ? systemLanguage() : code);
  recent_ = j.value("recent", std::vector<std::string>{});
  themes_.load(dataDir() + "/themes", configDir() + "themes");
  useTheme(j.value("theme", std::string("dark")), false);
  showCycles_ = j.value("show_cycles", true);
  zoom_ = std::clamp(j.value("zoom", j.value("editor_zoom", 1.0f)), 0.5f, 3.0f);
  tools_.loadSettings(j.value("tools", std::map<std::string, std::string>{}));
  // A new docked panel needs the default layout once (imgui.ini would keep it floating).
  if (j.value("layout_version", 0) < kLayoutVersion) {
    resetLayout_ = true;
    saveSettings();
  }
}

void App::saveSettings() {
  json j = {{"language", currentLanguage()},
            {"recent", recent_},
            {"show_cycles", showCycles_},
            {"zoom", zoom_},
            {"theme", themeId_},
            {"tools", tools_.userPaths()},
            {"layout_version", kLayoutVersion}};
  std::ofstream(configDir() + "settings.json") << j.dump(2) << "\n";
}

void App::useTheme(const std::string& id, bool save) {
  themeId_ = themes_.find(id) ? id : "dark";
  theme::Resolved r = themes_.resolve(themeId_);
  for (auto& p : r.problems) SDL_Log("Theme %s: %s", themeId_.c_str(), p.c_str());
  theme::setCurrent(r);
  theme::applyImGui(r);
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
  auto* ctx = static_cast<DialogCtx*>(userdata);
  if (files && files[0]) ctx->app->pushDialogResult(ctx->kind, files[0]);
  delete ctx;
}

void App::pushDialogResult(const std::string& kind, const std::string& path) {
  std::lock_guard<std::mutex> lock(dialogMutex_);
  dialogResults_.emplace_back(kind, path);
}

// ---------------------------------------------------------------------------
// Projects and files

void App::openPath(const std::string& path) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    for (auto& e : fs::directory_iterator(path, ec))
      if (e.path().extension() == ".vcsproj") return openProject(e.path().string());
    openFolder(path);
  } else if (fs::path(path).extension() == ".vcsproj") {
    openProject(path);
  } else if (fs::exists(path, ec)) {
    openFile(path);
  }
}

void App::openProject(const std::string& file) {
  Project p;
  std::string err;
  if (!p.load(file, &err)) {
    output_ += err + "\n";
    return;
  }
  project_ = p;
  folder_ = p.dir;
  symbols_.clear();
  builtFiles_.clear();
  diskSymbols_.clear();
  diagnostics_.clear();
  loadSymbols();
  loadUserState();
  addRecent(p.file);
  openFile(p.abs(p.main));
  SDL_SetWindowTitle(window_, ("VCSIDE - " + p.name).c_str());
}

void App::openFolder(const std::string& dir) {
  project_.reset();
  folder_ = fs::absolute(dir).string();
  addRecent(folder_);
  SDL_SetWindowTitle(window_, ("VCSIDE - " + fs::path(folder_).filename().string()).c_str());
}

OpenFile* App::openFile(const std::string& pathIn, int line) {
  std::string path = fs::weakly_canonical(pathIn).string();
  for (auto& o : files_) o->requestFocus = false;  // the file opened last is the one shown
  for (size_t i = 0; i < files_.size(); i++)
    if (files_[i]->path == path) {
      activeFile_ = (int)i;
      files_[i]->requestFocus = true;
      if (line > 0) files_[i]->editor.goTo(line - 1);
      return files_[i].get();
    }
  std::error_code ec;
  if (!fs::exists(path, ec)) return nullptr;
  auto f = std::make_unique<OpenFile>();
  f->path = path;
  if (lower(fs::path(path).extension().string()) == ".vcsgfx") {
    f->asmSyntax = false;
    f->gfx = std::make_unique<GfxEditor>();
    std::string err;
    if (!f->gfx->load(path, &err)) {
      output_ += F("Cannot read {0}: {1}", {path, err}) + "\n";
      return nullptr;
    }
    f->requestFocus = true;
    files_.push_back(std::move(f));
    activeFile_ = (int)files_.size() - 1;
    return files_.back().get();
  }
  f->asmSyntax = isAsmFile(path);
  f->editor.setText(readFile(path));
  f->editor.setGutterWidthChars(showCycles_ && f->asmSyntax ? 5 : 0);
  std::string ext = lower(fs::path(path).extension().string());
  bool cLike = ext == ".c" || ext == ".cpp" || ext == ".hpp" || (ext == ".h" && project_ && project_->language == Language::ElfC);
  if (cLike) f->asmSyntax = false;
  f->editor.setCommentPrefix(cLike ? "//" : ext == ".bas" ? "rem" : ";");
  if (f->asmSyntax || ext == ".bas") {
    EditorHooks h = ext == ".bas" ? basicHooks() : asmHooks();
    OpenFile* self = f.get();
    h.gotoDefinition = [this, self](const std::string& w) { return gotoDefinition(w, self->path, self->editor.cursor().line); };
    h.hasDefinition = [this, self](const std::string& w, int line) {
      std::string path;
      int at = 0;
      return findDefinition(w, self->path, line, path, at);
    };
    f->editor.setHooks(h);
  }
  f->requestFocus = true;
  if (line > 0) f->editor.goTo(line - 1);
  if (auto it = breakpoints_.find(path); it != breakpoints_.end()) f->editor.setBreakpoints(it->second);
  lint(*f);
  files_.push_back(std::move(f));
  activeFile_ = (int)files_.size() - 1;
  return files_.back().get();
}

gfx::Profile App::gfxProfile() {
  if (!project_ || project_->language != Language::BatariBasic) {
    gfx::Profile k;
    k.c = project_ && project_->language == Language::ElfC;
    return k;
  }
  std::string main = project_->abs(project_->main);
  std::error_code ec;
  auto t = fs::last_write_time(main, ec);
  if (main != gfxProfileFile_ || t != gfxProfileTime_) {
    gfxProfileFile_ = main;
    gfxProfileTime_ = t;
    gfxProfile_ = gfx::profileFromBasic(ec ? std::string() : readFile(main));
  }
  return gfxProfile_;
}
gfx::Tv App::gfxTv() const { return gfx::tvFromProject(project_ ? project_->tv : "NTSC"); }

bool App::saveFile(OpenFile& f) {
  if (f.gfx) {
    std::string err, generated;
    if (!f.gfx->save(f.path, gfxProfile(), &err, &generated)) {
      output_ += err + "\n";
      return false;
    }
    if (!generated.empty()) {
      lastStatus_ = F("Saved; the tables are in {0}", {fs::path(generated).filename().string()});
      // an open copy of the generated file shows the new tables
      std::string canon = fs::weakly_canonical(generated).string();
      for (auto& o : files_)
        if (o->path == canon && !o->gfx && !o->editor.dirty()) o->editor.setText(readFile(canon)), o->editor.markSaved();
    }
    updateBasicRegions(f);
    return true;
  }
  std::ofstream out(f.path, std::ios::binary);
  if (!out) {
    output_ += F("Cannot write {0}", {f.path}) + "\n";
    return false;
  }
  out << f.editor.text() << "\n";
  f.editor.markSaved();
  return true;
}

// The batari Basic programs of the project that hold regions of this graphics file
// (`rem vcsgfx <file> <item>` ... `rem vcsgfx end`) get the new blocks: in the editor if
// the program is open (saved too, unless it had changes of its own), on disk if not.
void App::updateBasicRegions(OpenFile& g) {
  std::string name = fs::path(g.path).filename().string();
  gfx::Profile k = g.gfx->profile(gfxProfile());
  fs::path root = project_ ? fs::path(project_->dir) : fs::path(g.path).parent_path();
  std::error_code ec;
  std::vector<std::string> programs;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    std::string fn = it->path().filename().string();
    if (it->is_directory(ec) && (fn.empty() || fn[0] == '.' || fn == "build" || it.depth() >= 3)) {
      it.disable_recursion_pending();
      continue;
    }
    if (lower(it->path().extension().string()) == ".bas") programs.push_back(fs::weakly_canonical(it->path(), ec).string());
  }
  std::vector<std::string> touched;
  for (auto& path : programs) {
    OpenFile* open = nullptr;
    for (auto& o : files_)
      if (o->path == path && !o->gfx) open = o.get();
    std::string text = open ? open->editor.text() : readFile(path);
    if (text.find("vcsgfx") == std::string::npos) continue;
    int n = 0;
    std::vector<std::string> problems;
    std::string updated = gfx::updateBasicRegions(text, name, g.gfx->asset(), k, &n, &problems);
    for (auto& p : problems) output_ += fs::path(path).filename().string() + ": " + p + "\n";
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
    for (auto& t : touched) list += (list.empty() ? "" : ", ") + t;
    lastStatus_ = F("Saved; graphics updated in {0}", {list});
  }
}

void App::saveAll() {
  for (auto& f : files_)
    if (f->dirty()) saveFile(*f);
}

// ---------------------------------------------------------------------------
// Build and run

void App::build(int thenRun) {
  if (building_) return;
  if (!project_) {
    output_ += std::string(T("No project open.")) + "\n";
    return;
  }
  saveAll();
  for (auto& id : project_->buildTools(tools_))
    if (Tool* t = tools_.get(id); t && !t->found()) {
      askForTool(id, T("build this project"), [this, thenRun] { build(thenRun); });
      return;
    }
  std::string err, workDir;
  EnvVars env;
  auto args = project_->buildArgs(tools_, &err, &workDir, &env);
  if (args.empty()) {
    output_ += err + "\n";
    lastStatus_ = err;
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
    std::error_code ec;
    for (auto suffix : {".bin", ".asm", ".list.txt", ".symbol.txt", ".lst", ".sym"})
      fs::remove(project_->basicOutput(suffix), ec);
  }
  job_.start(args, workDir, env);
}

void App::pollBuild() {
  if (!building_) return;
  output_ = job_.output();
  if (job_.running()) return;
  building_ = false;
  output_ = job_.output();
  bool gcc = project_ && (project_->language == Language::ElfC || !project_->buildCommand.empty());
  bool basic = project_ && project_->language == Language::BatariBasic && project_->buildCommand.empty();
  // the first line repeats the command ("> make ..."): only what it printed is parsed
  std::string printed = output_.rfind("> ", 0) == 0 && output_.find('\n') != std::string::npos ? output_.substr(output_.find('\n') + 1) : output_;
  if (basic) diagnostics_ = parseBasicOutput(printed, project_->main, readFile(project_->basicOutput(".asm")));
  else diagnostics_ = gcc ? parseGccOutput(printed) : parseDasmOutput(printed);
  if (gcc) {
    auto d = parseDasmOutput(printed);
    diagnostics_.insert(diagnostics_.end(), d.begin(), d.end());
  }
  bool ok = job_.exitCode() == 0;
  // batari Basic's script ends with 0 even when it fails: the ROM and no errors tell
  if (basic) ok = ok && finishBasicBuild();
  std::string buf = F(ok ? "Build succeeded (exit {0})" : "Build failed (exit {0})", {std::to_string(job_.exitCode())});
  output_ += "\n" + buf + "\n";
  lastStatus_ = buf;
  outputScroll_ = true;
  if (ok) {
    std::error_code ec;
    auto size = fs::file_size(project_->outputPath(), ec);
    if (!ec) output_ += F("{0}: {1} bytes", {project_->outputPath(), std::to_string(size)}) + "\n";
    loadSymbols();
    if (project_->language == Language::ElfC) measureElf();
    else if (basic) measureBasic(ec ? 0 : (long)size);
    else startSpace();
  }
  for (auto& f : files_) lint(*f);
  if (ok && runAfterBuild_) run(runAfterBuild_ == 2);
  runAfterBuild_ = 0;
}

// batari Basic leaves <main>.bas.bin, .list.txt and .symbol.txt next to the source: the
// ROM goes where the project says and the listing and symbols get the names Stella and
// VCSIDE look for (<rom stem>.lst and .sym).
bool App::finishBasicBuild() {
  std::error_code ec;
  std::string bin = project_->basicOutput(".bin");
  bool errors = std::any_of(diagnostics_.begin(), diagnostics_.end(), [](auto& d) { return d.severity == Diagnostic::Error; });
  if (errors || !fs::exists(bin, ec)) {
    if (!errors) diagnostics_.push_back({Diagnostic::Error, project_->main, 0, T("batari Basic did not write the ROM (see Output).")});
    return false;
  }
  auto copy = [&](const std::string& from, const std::string& to) {
    if (fs::equivalent(from, to, ec)) return true;
    fs::create_directories(fs::path(to).parent_path(), ec);
    return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  };
  if (!copy(bin, project_->outputPath())) {
    output_ += F("Cannot copy {0} to {1}: {2}", {bin, project_->outputPath(), ec.message()}) + "\n";
    return false;
  }
  copy(project_->basicOutput(".list.txt"), project_->stem() + ".lst");
  copy(project_->basicOutput(".symbol.txt"), project_->stem() + ".sym");
  return true;
}

void App::measureBasic(long romSize) {
  SpaceOptions opt;
  opt.warnPercent = project_->spaceWarnPercent;
  space_ = parseBasicSpace(output_, romSize, opt);
  if (space_->banks.empty()) {
    space_.reset();
    spaceNote_ = T("batari Basic did not say how much ROM is left.");
    return;
  }
  for (auto d : space_->diagnostics) {
    d.message = T("Space") + std::string(": ") + d.message;
    diagnostics_.push_back(d);
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
  std::string rom = project_->outputPath(), cc = project_->stem() + "-chameleon.elf";
  ElfFile r, c;
  std::string err;
  if (!readElf(readFile(rom), r, &err) || r.type != 1) {
    spaceNote_ = F("Cannot measure {0}: {1}", {rom, err.empty() ? T("it is not a relocatable ELF object") : err});
    return;
  }
  std::error_code ec;
  bool useCc = fs::exists(cc, ec) && fs::last_write_time(cc, ec) >= fs::last_write_time(rom, ec) &&
               readElf(readFile(cc), c, &err) && c.type == 2;
  elfSpace_ = analyzeElfSpace(r, (long)fs::file_size(rom, ec), useCc ? &c : nullptr, project_->spaceWarnPercent);
  for (auto d : elfSpace_->diagnostics) {
    d.message = T("Space") + std::string(": ") + d.message;
    diagnostics_.push_back(d);
  }
  output_ += elfSpace_->summary() + "\n";
}

void App::pollSpace() {
  if (!spaceRunning_ || spaceJob_.running()) return;
  spaceRunning_ = false;
  if (!project_ || spaceJob_.exitCode() != 0) {
    spaceNote_ = T("Could not measure the space: DASM failed writing the block file.");
    return;
  }
  std::error_code ec;
  long romSize = (long)fs::file_size(project_->outputPath(), ec);
  if (ec) romSize = 0;
  std::string rasPath = project_->stem() + ".ras";
  auto rom = parseDasmRas(readFile(rasPath));
  fs::remove(rasPath, ec);
  std::string lst = readFile(project_->stem() + ".lst");
  // Listing file names are as DASM opened them: absolute, or relative to the project or an include folder.
  auto readSource = [this](const std::string& name) {
    std::vector<std::string> dirs = {project_->dir};
    for (auto& d : project_->includeDirs) dirs.push_back(project_->abs(d));
    fs::path p(name);
    std::error_code e;
    if (!p.is_absolute())
      for (auto& d : dirs)
        if (fs::exists(fs::path(d) / name, e)) {
          p = fs::path(d) / name;
          break;
        }
    std::vector<std::string> lines;
    std::istringstream in(readFile(p.string()));
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    return lines;
  };
  auto ram = parseDasmListingRam(lst, readSource, [this](const std::string& s) { return lookupSymbol(s); });
  auto code = parseDasmListingCode(lst);
  FormatSpace fmt;
  if (const CartFormat* f = kb_.format(project_->format)) fmt = f->space;
  SpaceOptions opt;
  opt.warnPercent = project_->spaceWarnPercent;
  opt.stackReserve = project_->stackReserve;
  space_ = analyzeSpace(rom, romSize, ram, code, fmt, opt);
  if (!fmt.known) spaceNote_ = F("The {0} format has no space data yet: 4 KB banks assumed.", {project_->format});
  for (auto d : space_->diagnostics) {
    d.message = T("Space") + std::string(": ") + d.message;
    diagnostics_.push_back(d);
  }
  output_ += space_->summary() + "\n";
  outputScroll_ = true;
  for (auto& f : files_) lint(*f);
}

void App::run(bool debug) {
  if (!project_) return;
  if (Tool* t = tools_.get(project_->emulator); t && !t->found()) {
    askForTool(t->id, T("run the game"), [this, debug] { run(debug); });
    return;
  }
  std::string err;
  auto args = project_->runArgs(tools_, debug, &err);
  if (debug && !args.empty()) {
    auto extra = debugArgs(project_->emulator);
    args.insert(args.end() - 1, extra.begin(), extra.end());  // before the ROM
  }
  EmulatorLaunch l;
  l.args = args;
  if (!args.empty() && !prepareEmulatorLaunch(*project_, kb_, (fs::path(configDir()) / "emulators").string(), l, &err))
    args.clear();
  for (auto& n : l.notes) output_ += n + "\n";
  if (args.empty() || !launchDetached(l.args, l.workDir, &err, l.env)) {
    output_ += err + "\n";
    lastStatus_ = err;
  } else {
    std::string envText;
    for (auto& [k, v] : l.env) envText += k + "=" + quoteArgs({v}) + " ";
    output_ += "> " + envText + quoteArgs(l.args) + "\n";
  }
  outputScroll_ = true;
}

void App::loadSymbols() {
  if (!project_) return;
  std::string sym = project_->stem() + ".sym";
  std::error_code ec;
  if (fs::exists(sym, ec)) symbols_ = parseDasmSymbols(readFile(sym));
  builtFiles_.clear();
  liveReady_ = false;  // the next scan includes these sources
  if (fs::exists(project_->stem() + ".lst", ec)) {
    std::istringstream in(readFile(project_->stem() + ".lst"));
    std::set<std::string> seen;
    for (std::string l; std::getline(in, l);) {
      if (l.rfind("------- FILE ", 0) != 0) continue;
      std::string name = l.substr(13, l.find(" LEVEL ") == std::string::npos ? std::string::npos : l.find(" LEVEL ") - 13);
      std::string path = resolveSource(name);
      if (!fs::is_regular_file(path, ec)) {  // a source that cannot be read: hide nothing
        builtFiles_.clear();
        break;
      }
      if (seen.insert(path).second) builtFiles_.push_back(path);
    }
  }
  addresses_ = AddressMap();
  std::string lst = project_->stem() + ".lst";
  bool basic = project_->language == Language::BatariBasic;
  if ((project_->language == Language::Dasm || basic) && fs::exists(lst, ec)) {
    long bank = 4096;
    if (const CartFormat* f = kb_.format(project_->format); f && f->space.known) bank = f->space.bankSize;
    addresses_.load(readFile(lst), [this](const std::string& s) { return lookupSymbol(s); }, bank);
    // the listing is of the generated .asm: its ";;line N;;" marks give the lines of the .bas
    if (basic)
      addresses_.remap(fs::path(project_->main).filename().string() + ".asm",
                       basicLineMap(readFile(project_->basicOutput(".asm"))), project_->abs(project_->main));
    addresses_.resolveFiles([this](const std::string& n) { return resolveSource(n); });
  }
}

std::string App::resolveSource(const std::string& name) const {
  fs::path p(name);
  std::error_code ec;
  if (!p.is_absolute() && project_) {
    std::vector<std::string> dirs = {project_->dir};
    for (auto& d : project_->includeDirs) dirs.push_back(project_->abs(d));
    for (auto& d : dirs)
      if (fs::exists(fs::path(d) / name, ec)) {
        p = fs::path(d) / name;
        break;
      }
  }
  return fs::weakly_canonical(p, ec).string();
}

// ---------------------------------------------------------------------------
// Breakpoints and debugging

void App::loadUserState() {
  breakpoints_.clear();
  if (!project_) return;
  json j;
  try {
    std::ifstream f(project_->file + ".user");
    if (f) j = json::parse(f);
  } catch (...) {
  }
  if (!j.is_object() || !j.contains("breakpoints")) return;
  for (auto& [file, lines] : j["breakpoints"].items()) {
    std::set<int> s;
    for (auto& l : lines)
      if (l.is_number_integer() && l.get<int>() > 0) s.insert(l.get<int>() - 1);
    std::error_code ec;
    if (!s.empty()) breakpoints_[fs::weakly_canonical(fs::path(project_->dir) / file, ec).string()] = s;
  }
}

void App::saveUserState() {
  if (!project_) return;
  json bps = json::object();
  for (auto& [file, lines] : breakpoints_) {
    if (lines.empty()) continue;
    std::error_code ec;
    auto rel = fs::relative(file, project_->dir, ec);
    json arr = json::array();
    for (int l : lines) arr.push_back(l + 1);
    bps[ec ? file : rel.generic_string()] = arr;
  }
  std::ofstream(project_->file + ".user") << json{{"breakpoints", bps}}.dump(2) << "\n";
}

// Sends the editor's breakpoints to the emulator: Gopher2600 reads a script given
// with -script, Stella the one named after the ROM in its user folder.
std::vector<std::string> App::debugArgs(const std::string& emulator) {
  for (auto& f : files_)
    if (!f->editor.breakpoints().empty() || breakpoints_.count(f->path)) breakpoints_[f->path] = f->editor.breakpoints();
  if (project_->language == Language::ElfC) {
    // C on the ARM: the emulators have no breakpoints by source line for it
    bool any = std::any_of(breakpoints_.begin(), breakpoints_.end(), [](auto& b) { return !b.second.empty(); });
    if (any) output_ += std::string(T("Breakpoints are not passed for ELF projects: the emulators cannot stop on a line of C.")) + "\n";
    return {};
  }
  std::vector<Breakpoint> bps;
  for (auto& [file, lines] : breakpoints_)
    for (int l : lines) {
      auto isCode = [this](const ListedLine& ll) {
        // the listing keeps the label in its own column: look for a word that is an instruction
        std::istringstream w(ll.source);
        for (std::string t; w >> t;) {
          if (t[0] == ';') break;
          std::string m = upper(t.substr(0, t.find('.')));
          if (kb_.instruction(m)) return true;
        }
        return false;
      };
      std::string rel = fs::path(file).filename().string();
      if (addresses_.empty()) {
        output_ += F("{0}:{1}: no listing to place the breakpoint: build first.", {rel, std::to_string(l + 1)}) + "\n";
        continue;
      }
      auto bp = addresses_.breakpointAt(file, l + 1, isCode);
      if (!bp) {
        output_ += F("{0}:{1}: no code at or after this line; breakpoint ignored.", {rel, std::to_string(l + 1)}) + "\n";
        continue;
      }
      char where[48];
      snprintf(where, sizeof where, "$%04lX", bp->cpu);
      std::string bank = addresses_.banks() > 1 ? " " + F("(bank {0})", {std::to_string(bp->bank)}) : "";
      if (bp->at->line != l + 1)
        output_ += F("{0}:{1}: breakpoint at {2}{3}, the code of line {4}.", {rel, std::to_string(l + 1), where, bank, std::to_string(bp->at->line)}) + "\n";
      else
        output_ += F("{0}:{1}: breakpoint at {2}{3}.", {rel, std::to_string(l + 1), where, bank}) + "\n";
      bps.push_back(*bp);
    }
  std::vector<std::string> args;
  std::error_code ec;
  if (emulator == "gopher2600") {
    std::string path = project_->stem() + ".gopher2600.script";
    if (bps.empty()) {
      fs::remove(path, ec);
      return args;
    }
    std::ofstream out(path);
    for (auto& b : bps) {
      char line[64];
      if (addresses_.banks() > 1) snprintf(line, sizeof line, "BREAK PC $%04lX & BANK %d\n", b.cpu, b.bank);
      else snprintf(line, sizeof line, "BREAK PC $%04lX\n", b.cpu);
      out << line;
    }
    args = {"-script", path};
  } else if (emulator == "stella") {
    // Stella's debugger runs <userdir>/<rom name>.script when it opens, and
    // VCSIDE launches it with -userdir set to the build folder.
    fs::remove(project_->stem() + ".stella.script", ec);  // VCSIDE 0.1 wrote this one
    std::string path = project_->stem() + ".script";
    if (bps.empty()) {
      fs::remove(path, ec);
      return args;
    }
    std::ofstream out(path);
    for (auto& b : bps) {
      char line[64];
      snprintf(line, sizeof line, "break $%04lX %d\n", b.cpu, b.bank);
      out << line;
    }
  }
  return args;
}


std::optional<int> App::lookupSymbol(const std::string& name) const {
  // a constant as the source says it now goes before the last build's value
  if (auto lv = liveValues_.find(name); lv != liveValues_.end()) return lv->second;
  auto it = symbols_.find(name);
  if (it != symbols_.end() && !builtGone(name)) return it->second;
  if (const Register* r = kb_.reg(name)) {
    int a = r->addr();
    if (a >= 0) return r->access == "R" && a < 0x30 ? a + 0x30 : a;
  }
  return std::nullopt;
}

static std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(l);
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
  auto add = [&](const std::string& file, const std::vector<SourceSymbol>& syms) {
    for (auto& sym : syms)
      if (sym.name[0] != '.') live.emplace(sym.name, LiveSymbol{sym, file});  // the first definition wins
  };
  std::set<std::string> open;
  for (auto& f : files_)
    if (f->asmSyntax) {
      add(f->path, scanAsmSymbols(f->editor.lines()));
      open.insert(f->path);
    }
  // the other sources of the folder, read again only when their date changes
  if ((!folder_.empty() || !builtFiles_.empty()) && (force || now - diskTicks_ > 3000)) {
    diskTicks_ = now;
    bool cHeaders = project_ && project_->language == Language::ElfC;
    std::map<std::string, ScannedFile> seen;
    std::error_code ec;
    if (!folder_.empty()) {
      for (auto it = fs::recursive_directory_iterator(folder_, fs::directory_options::skip_permission_denied, ec);
           it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (name == "build" || (!name.empty() && name[0] == '.'))) {
          it.disable_recursion_pending();
          continue;
        }
        if (!it->is_regular_file(ec) || !isAsmFile(it->path().string()) || it->file_size(ec) > 4 * 1024 * 1024) continue;
        if (cHeaders && lower(it->path().extension().string()) == ".h") continue;  // C, not assembly
        std::string path = fs::weakly_canonical(it->path(), ec).string();  // as the open files have it
        long long mtime = (long long)it->last_write_time(ec).time_since_epoch().count();
        auto old = diskSymbols_.find(path);
        if (old != diskSymbols_.end() && old->second.mtime == mtime) {
          seen[path] = std::move(old->second);
          continue;
        }
        seen[path] = {mtime, scanAsmSymbols(splitLines(readFile(path)))};
      }
    }
    for (auto& path : builtFiles_) {
      if (seen.count(path)) continue;
      long long mtime = (long long)fs::last_write_time(path, ec).time_since_epoch().count();
      if (ec) continue;
      auto old = diskSymbols_.find(path);
      if (old != diskSymbols_.end() && old->second.mtime == mtime) {
        seen[path] = std::move(old->second);
        continue;
      }
      seen[path] = {mtime, scanAsmSymbols(splitLines(readFile(path)))};
    }
    diskSymbols_ = std::move(seen);
  }
  for (auto& [path, f] : diskSymbols_)
    if (!open.count(path)) add(path, f.syms);
  liveSymbols_ = std::move(live);
  liveReady_ = true;
  // constants that refer to other constants: a few passes
  liveValues_.clear();
  for (int pass = 0; pass < 6; pass++) {
    bool more = false;
    for (auto& [name, l] : liveSymbols_) {
      if (l.sym.kind != SourceSymbol::Constant || liveValues_.count(name)) continue;
      const std::string& self = name;
      auto v = evalExpr(l.sym.value, [this, &self](const std::string& x) -> std::optional<int> {
        if (x == self) return std::nullopt;
        return lookupSymbol(x);
      });
      if (v) liveValues_[name] = *v, more = true;
    }
    if (!more) break;
  }
}

// "label (main.asm:12)", "constant = $80 + 2" or "macro (kernel.h:40)"; full = for the tooltip
std::string App::liveSymbolText(const LiveSymbol& l, bool full) const {
  std::string where = fs::path(l.file).filename().string() + ":" + std::to_string(l.sym.line + 1);
  bool here = activeFile_ >= 0 && files_[activeFile_]->path == l.file;
  if (l.sym.kind == SourceSymbol::Macro) return full ? F("{0}: macro ({1})", {l.sym.name, where}) : T("macro") + (here ? "" : " · " + where);
  if (l.sym.kind == SourceSymbol::Constant) {
    std::string s = full ? l.sym.name + " = " + l.sym.value : "= " + l.sym.value;
    if (auto v = liveValues_.find(l.sym.name); v != liveValues_.end() && full) {
      char b[40];
      snprintf(b, sizeof b, "  ($%04X, %d)", v->second & 0xFFFF, v->second);
      s += b;
    }
    if (full) s += "\n" + where;
    return s;
  }
  if (full) return F("{0}: label ({1}). Build to get its address.", {l.sym.name, where});
  return T("label") + (here ? "" : " · " + where);
}

std::string App::testComplete(const std::string& word) {
  if (activeFile_ < 0) return "no file\n";
  refreshLiveSymbols(true);
  EditorHooks h = files_[activeFile_]->asmSyntax ? asmHooks() : basicHooks();
  std::string s;
  for (auto& c : h.complete(word, "")) s += c.word + "\t" + c.detail + "\n";
  return s + "hover: " + h.hover(word, "") + "\n";
}

// A file named by a tool: absolute, relative to the project or an include folder, or
// (ld gives just "main.c") the only file with that name in the project.
std::string App::findProjectFile(const std::string& name) const {
  std::error_code ec;
  fs::path p(name);
  if (p.is_absolute()) return fs::exists(p, ec) ? p.string() : "";
  if (!project_) return "";
  std::vector<std::string> dirs = {project_->dir};
  for (auto& d : project_->includeDirs) dirs.push_back(project_->abs(d));
  for (auto& d : dirs)
    if (fs::exists(fs::path(d) / p, ec)) return fs::weakly_canonical(fs::path(d) / p, ec).string();
  if (p.has_parent_path()) return "";
  for (auto it = fs::recursive_directory_iterator(project_->dir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_directory(ec) && (it->path().filename() == "build" || it->path().filename().string()[0] == '.')) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->path().filename() == p) return it->path().string();
  }
  return "";
}

// Build diagnostics for this file plus the illegal-opcode policy of the project.
void App::lint(OpenFile& f) {
  std::map<int, std::pair<bool, std::string>> m;
  for (auto& d : diagnostics_) {
    if (d.line <= 0) continue;
    fs::path p(d.file);
    if (!p.is_absolute() && project_) p = fs::path(project_->dir) / p;
    std::error_code ec;
    if (fs::weakly_canonical(p, ec).string() != f.path && fs::path(d.file).filename() != fs::path(f.path).filename()) continue;
    auto& e = m[d.line];
    e.first = e.first || d.severity == Diagnostic::Error;
    e.second += (e.second.empty() ? "" : "\n") + d.message;
  }
  if (f.asmSyntax) {
    IllegalOpcodes pol = project_ ? project_->illegal : IllegalOpcodes::Warn;
    const auto& lines = f.editor.lines();
    for (int i = 0; i < (int)lines.size() && pol != IllegalOpcodes::Allow; i++) {
      AsmLine a = splitAsmLine(lines[i]);
      std::string mn = upper(a.mnemonic.substr(0, a.mnemonic.find('.')));
      const Instruction* in = kb_.instruction(mn);
      if (!in || !in->illegal) continue;
      bool stable = !in->opcodes.empty() && in->opcodes[0]->stable;
      // NOP is legal by name; its illegal forms only matter with operands.
      if (pol == IllegalOpcodes::StableOnly && stable) continue;
      bool error = pol == IllegalOpcodes::Forbid || (pol == IllegalOpcodes::StableOnly && !stable);
      auto& e = m[i + 1];
      e.first = e.first || error;
      e.second += (e.second.empty() ? "" : "\n") + F(stable ? "Illegal opcode: {0}" : "Unstable illegal opcode: {0}", {mn});
    }
  }
  f.editor.setMarkers(std::move(m));
}

// ---------------------------------------------------------------------------
// Assembler syntax

std::vector<Token> App::tokenizeAsm(const std::string& l) const {
  std::vector<Token> t;
  int n = (int)l.size(), i = 0;
  bool first = true;  // first word of the line (label if at column 0)
  bool sawMnemonic = false;
  while (i < n) {
    char c = l[i];
    if (c == ';') {
      t.push_back({i, n, TokenKind::Comment});
      break;
    }
    if (std::isspace((unsigned char)c)) { i++; continue; }
    int s = i;
    if (c == '"') {
      i++;
      while (i < n && l[i] != '"') i++;
      i = std::min(n, i + 1);
      t.push_back({s, i, TokenKind::String});
    } else if (c == '$' || c == '%' || c == '#' || std::isdigit((unsigned char)c)) {
      if (c == '#') i++;
      if (i < n && (l[i] == '<' || l[i] == '>')) i++;
      int ns = i;
      if (i < n && (l[i] == '$' || l[i] == '%')) i++;
      while (i < n && std::isxdigit((unsigned char)l[i])) i++;
      if (i == ns) {  // '#' followed by a symbol
        t.push_back({s, i, TokenKind::Number});
        continue;
      }
      t.push_back({s, i, TokenKind::Number});
    } else if (std::isalpha((unsigned char)c) || c == '_' || c == '.') {
      while (i < n && (std::isalnum((unsigned char)l[i]) || l[i] == '_' || l[i] == '.')) i++;
      std::string w = l.substr(s, i - s);
      std::string u = upper(w);
      std::string base = u.substr(0, u.find('.', 1));
      TokenKind k = TokenKind::Symbol;
      if (first && s == 0) k = TokenKind::Label;
      else if (!sawMnemonic) {
        if (const Instruction* in = kb_.instruction(base)) {
          k = !in->illegal ? TokenKind::Mnemonic
              : (!in->opcodes.empty() && in->opcodes[0]->stable) ? TokenKind::IllegalMnemonic
                                                                 : TokenKind::UnstableMnemonic;
          sawMnemonic = true;
        } else {
          for (auto& d : dasmDirectives())
            if (u == d) { k = TokenKind::Directive; sawMnemonic = true; break; }
        }
      } else if (kb_.reg(w)) {
        k = TokenKind::Register;
      }
      t.push_back({s, i, k});
      first = false;
      continue;
    } else {
      i++;
    }
    first = false;
  }
  return t;
}

EditorHooks App::asmHooks() {
  EditorHooks h;
  h.tokenize = [this](const std::string& l) { return tokenizeAsm(l); };
  h.gutter = [this](int, const std::string& l) -> std::string {
    LineCycles c = countLine(kb_, l, [this](const std::string& s) { return lookupSymbol(s); });
    if (!c.isCode) return {};
    if (!c.known) return "?";
    std::string s = std::to_string(c.min);
    if (c.max != c.min) s += "/" + std::to_string(c.max);
    if (c.guessed) s += "~";
    return s;
  };
  h.hover = [this](const std::string& word, const std::string& line) -> std::string {
    std::string u = upper(word.substr(0, word.find('.', 1)));
    if (const Instruction* in = kb_.instruction(u)) {
      std::string s = u + " - " + in->summary;
      if (in->illegal) s += std::string("  ") + (in->opcodes.empty() || in->opcodes[0]->stable ? T("[ILLEGAL]") : T("[ILLEGAL, UNSTABLE]"));
      if (!in->flags.empty()) s += "\n" + F("Flags: {0}", {in->flags});
      s += "\n" + in->description;
      LineCycles c = countLine(kb_, line, [this](const std::string& x) { return lookupSymbol(x); });
      if (c.known && c.mnemonic == u) {
        std::string cyc = std::to_string(c.min) + (c.max != c.min ? "-" + std::to_string(c.max) : "");
        s += "\n\n" + F("This line: {0}, {1} bytes, {2} cycles", {c.mode, std::to_string(c.bytes), cyc});
        if (c.guessed) s += " " + Ts("(zero page/absolute guessed: unknown symbol, build to resolve)");
      }
      return s;
    }
    if (const Register* r = kb_.reg(word)) return registerHelp(*r);
    refreshLiveSymbols();
    auto live = liveSymbols_.find(word);
    // a macro, or a constant that evaluates now: the source says more than the last build
    if (live != liveSymbols_.end() && (live->second.sym.kind == SourceSymbol::Macro ||
                                       (live->second.sym.kind == SourceSymbol::Constant && liveValues_.count(word))))
      return liveSymbolText(live->second, true);
    auto it = symbols_.find(word);
    if (it != symbols_.end() && !builtGone(word)) {
      char b[48];
      snprintf(b, sizeof b, "%s = $%04X (%d)", word.c_str(), it->second, it->second);
      return b;
    }
    if (live != liveSymbols_.end()) return liveSymbolText(live->second, true);
    if (!word.empty() && word[0] == '.' && activeFile_ >= 0)
      for (auto& sym : scanAsmSymbols(files_[activeFile_]->editor.lines()))
        if (sym.name == word) return liveSymbolText({sym, files_[activeFile_]->path}, true);
    return {};
  };
  h.complete = [this](const std::string& prefix, const std::string&) {
    std::vector<Completion> out;
    for (auto& e : kb_.complete(prefix, 40)) {
      TokenKind k = e.kind == KbEntry::Mnemonic ? TokenKind::Mnemonic
                    : e.kind == KbEntry::Register ? TokenKind::Register
                                                   : TokenKind::Directive;
      if (e.kind == KbEntry::Mnemonic) {
        const Instruction* in = kb_.instruction(e.word);
        if (in && in->illegal) {
          if (project_ && project_->illegal == IllegalOpcodes::Forbid) continue;
          bool stable = !in->opcodes.empty() && in->opcodes[0]->stable;
          if (!stable && project_ && project_->illegal == IllegalOpcodes::StableOnly) continue;
          k = stable ? TokenKind::IllegalMnemonic : TokenKind::UnstableMnemonic;
        }
      }
      out.push_back({e.word, e.summary, k});
    }
    // the sources as they are now (labels, constants and macros not built yet), then the
    // last build's symbols; the active file's .local labels too
    std::string p = upper(prefix);
    auto matches = [&](const std::string& name) { return name != prefix && upper(name).compare(0, p.size(), p) == 0; };
    refreshLiveSymbols();
    std::set<std::string> seen;
    for (auto& o : out) seen.insert(o.word);
    auto push = [&](const std::string& name, const std::string& detail, TokenKind k) {
      if (out.size() < 60 && seen.insert(name).second) out.push_back({name, detail, k});
    };
    if (activeFile_ >= 0 && !prefix.empty() && prefix[0] == '.')
      for (auto& sym : scanAsmSymbols(files_[activeFile_]->editor.lines()))
        if (sym.name[0] == '.' && matches(sym.name)) push(sym.name, T("local label"), TokenKind::Label);
    for (auto& [name, l] : liveSymbols_)
      if (matches(name))
        push(name, liveSymbolText(l, false),
             l.sym.kind == SourceSymbol::Macro ? TokenKind::Directive : l.sym.kind == SourceSymbol::Label ? TokenKind::Label : TokenKind::Symbol);
    for (auto& [name, v] : symbols_)
      if (matches(name) && !builtGone(name)) push(name, T("symbol"), TokenKind::Symbol);
    return out;
  };
  return h;
}

std::string App::registerHelp(const Register& r) const {
  std::string s = r.name + " (" + r.chip + " " + r.address + ", " + (r.access == "R" ? T("read") : T("write")) + ")  " + r.bits + "\n" + r.summary;
  if (!r.timing.empty()) s += "\n" + F("Timing: {0}", {r.timing});
  for (auto& i : r.issues) s += "\n! " + i;
  return s;
}

// batari Basic: its keywords and variables, the registers, and the labels of the file.
EditorHooks App::basicHooks() {
  EditorHooks h;
  h.tokenize = [this](const std::string& l) { return tokenizeBasic(kb_, l); };
  h.hover = [this](const std::string& word, const std::string& line) -> std::string {
    std::string w = word;
    size_t at = line.find(word);
    if (at != std::string::npos && line.compare(at + word.size(), 1, ":") == 0 && kb_.basic(word + ":")) w += ":";
    if (const BasicKeyword* b = kb_.basic(w)) return b->syntax + "\n" + b->summaryText();
    if (const Register* r = kb_.reg(word)) return registerHelp(*r);
    return {};
  };
  h.complete = [this](const std::string& prefix, const std::string&) {
    std::vector<Completion> out;
    if (prefix.empty()) return out;
    std::string p = lower(prefix);
    for (auto& [w, b] : kb_.basicKeywords())
      if (w.compare(0, p.size(), p) == 0 && out.size() < 40)
        out.push_back({w, b.summaryText(), b.kind == "variable" ? TokenKind::Register
                                           : b.kind == "directive" || b.kind == "block" ? TokenKind::Directive
                                                                                         : TokenKind::Mnemonic});
    std::string u = upper(prefix);
    for (auto& r : kb_.registers())
      if (r.name.compare(0, u.size(), u) == 0 && out.size() < 50) out.push_back({r.name, r.summary, TokenKind::Register});
    if (activeFile_ >= 0) {
      std::set<std::string> seen;
      for (auto& l : files_[activeFile_]->editor.lines()) {
        std::string lab = basicLabelOf(l);
        if (!lab.empty() && lab != prefix && lab.compare(0, prefix.size(), prefix) == 0 && seen.insert(lab).second && out.size() < 60)
          out.push_back({lab, T("label"), TokenKind::Label});
      }
    }
    return out;
  };
  return h;
}

bool App::testMenu(const std::string& path) {
  if (nativeMenu_) return nativeMenuPerform(path);
  MenuBar bar = buildMenuBar(false);
  const std::vector<MenuItem>* level = &bar;
  size_t start = 0;
  for (;;) {
    size_t slash = path.find('/', start);
    std::string name = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    auto it = std::find_if(level->begin(), level->end(), [&](const MenuItem& m) { return !m.separator && m.label == name; });
    if (it == level->end() || !it->enabled) return false;
    if (slash == std::string::npos) {
      if (!it->action) return false;
      it->action();
      return true;
    }
    level = &it->children;
    start = slash + 1;
  }
}

// The steps of the zoom, as in most editors.
static const float kZoomSteps[] = {0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};

void App::zoomEditor(int steps) {
  float z = 1.0f;
  if (steps != 0) {
    int n = (int)(sizeof kZoomSteps / sizeof kZoomSteps[0]), at = 0;
    for (int i = 0; i < n; i++)
      if (std::fabs(kZoomSteps[i] - zoom_) < std::fabs(kZoomSteps[at] - zoom_)) at = i;
    z = kZoomSteps[std::clamp(at + steps, 0, n - 1)];
  }
  if (z == zoom_) return;
  zoom_ = z;
  lastStatus_ = F("Zoom: {0} %", {std::to_string((int)(z * 100 + 0.5f))});
  saveSettings();
}

ImFont* App::editorFont() const { return mono_; }

void App::testSelect(int a, int b) {
  if (activeFile_ < 0) return;
  auto& ed = files_[activeFile_]->editor;
  ed.goTo(a - 1);
  ed.selectLines(a - 1, b - 1);
}

// Is `line` the definition of `word`? (label at column 0, "word = x", "word equ x", "mac word")
static bool definesWord(const std::string& line, const std::string& word) {
  AsmLine a = splitAsmLine(line);
  if (!a.label.empty()) return a.label == word;
  if (upper(a.mnemonic) == "MAC" || upper(a.mnemonic) == "MACRO") {
    std::string n = a.operand.substr(0, a.operand.find_first_of(" \t,"));
    return n == word;
  }
  return false;
}

bool App::findDefinition(const std::string& word, const std::string& fromFile, int fromLine, std::string& path, int& line) {
  if (word.empty()) return false;
  bool bas = lower(fs::path(fromFile).extension().string()) == ".bas";
  auto defines = [&](const std::string& line) { return bas ? basicLabelOf(line) == word : definesWord(line, word); };
  auto searched = [&](const std::string& path) { return bas ? lower(fs::path(path).extension().string()) == ".bas" : isAsmFile(path); };
  // 1. The open file: the closest definition above the cursor (local .labels repeat
  //    per SUBROUTINE), otherwise the first one below.
  for (auto& f : files_) {
    if (f->path != fromFile) continue;
    const auto& lines = f->editor.lines();
    int best = -1;
    for (int i = 0; i < (int)lines.size(); i++)
      if (defines(lines[i]) && (best < 0 || i <= fromLine)) best = i;
    if (best >= 0) {
      path = f->path, line = best + 1;
      return true;
    }
  }
  // 2. Other source files of the project (not build output).
  if (folder_.empty()) return false;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(folder_, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    std::string name = it->path().filename().string();
    if (it->is_directory() && (name == "build" || (!name.empty() && name[0] == '.'))) {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file() || !searched(it->path().string()) || it->path().string() == fromFile) continue;
    std::ifstream in(it->path());
    std::string text;
    for (int n = 1; std::getline(in, text); n++) {
      if (!text.empty() && text.back() == '\r') text.pop_back();
      if (defines(text)) {
        path = it->path().string(), line = n;
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

void App::requestQuit() {
  for (auto& f : files_)
    if (f->dirty()) {
      quitConfirm_ = true;
      return;
    }
  quit_ = true;
}

void App::frame() {
  {
    std::lock_guard<std::mutex> lock(dialogMutex_);
    while (!dialogResults_.empty()) {
      auto [kind, path] = dialogResults_.front();
      dialogResults_.pop_front();
      if (kind == "project") openPath(path);
      else if (kind == "folder") openPath(path);
      else if (kind == "file") openFile(path);
      else if (kind == "newdir") npDir_ = path;
      else if (kind == "themeimport") {
        std::string id, err;
        if (themes_.import(path, &id, &err)) useTheme(id), themeEditor_.open = true;
        else lastStatus_ = err;
      } else if (kind.rfind("themeexport:", 0) == 0) {
        std::string p = path;
        if (lower(fs::path(p).extension().string()) != ".vcstheme") p += ".vcstheme";
        std::ofstream out(p, std::ios::binary);
        if (out << themes_.exportJson(kind.substr(12))) lastStatus_ = F("Theme saved in {0}", {p});
        else lastStatus_ = F("Cannot write {0}", {p});
      }
      else if (kind == "newgfx") {
        std::string p = path;
        if (lower(fs::path(p).extension().string()) != ".vcsgfx") p += ".vcsgfx";
        GfxEditor g;
        std::string err;
        g.asset() = GfxEditor::blank();
        if (g.save(p, gfxProfile(), &err)) openFile(p);
        else output_ += err + "\n";
      }
      else if (kind.rfind("tool:", 0) == 0) {
        if (Tool* t = tools_.get(kind.substr(5))) t->userPath = path;
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
    auto& ed = files_[activeFile_]->editor;
    const std::string& l = ed.lines()[ed.cursor().line];
    int s = ed.cursor().col, e = s;
    while (s > 0 && (std::isalnum((unsigned char)l[s - 1]) || l[s - 1] == '_')) s--;
    while (e < (int)l.size() && (std::isalnum((unsigned char)l[e]) || l[e] == '_')) e++;
    if (e > s) showDoc(l.substr(s, e - s));
  }
  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G, false) && !addresses_.empty()) showGoToAddress_ = true;
  // zoom (Ctrl/Cmd + "+", "-", "0"): main.cpp, from the SDL key events
  if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
    if (io.KeyShift) saveAll();
    else if (activeFile_ >= 0) saveFile(*files_[activeFile_]);
  }

  menuBar();
  dockspace();
  projectPanel();
  editorsPanel();
  outputPanel();
  problemsPanel();
  spacePanel();
  docsPanel();
  newProjectDialog();
  toolsDialog();
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
                       std::error_code ec;
                       fs::create_directories(dir, ec);
                       SDL_OpenURL(("file://" + fs::absolute(dir).string()).c_str());
                     }});
  aboutDialog();
  missingToolDialog();
  closeDialogs();
  goToAddressDialog();
  emulatorOptionsDialog();
}

// The menu bar as data: ImGui draws it on Windows and Linux, macOS puts it in the
// system menu bar. Actions look up the active file when they run.
MenuBar App::buildMenuBar(bool native) {
  auto item = [](const std::string& label, const std::string& shortcut, std::function<void()> action,
                 bool enabled = true, bool checked = false) {
    MenuItem m;
    m.label = label, m.shortcut = shortcut, m.action = std::move(action), m.enabled = enabled, m.checked = checked;
    return m;
  };
  auto sub = [](const std::string& label, std::vector<MenuItem> children, bool enabled = true) {
    MenuItem m;
    m.label = label, m.children = std::move(children), m.enabled = enabled;
    return m;
  };
  auto editor = [this]() -> TextEditor* { return activeFile_ >= 0 ? &files_[activeFile_]->editor : nullptr; };
  auto onEditor = [editor](void (*f)(TextEditor&)) { return [editor, f] { if (TextEditor* e = editor()) f(*e); }; };
  bool hasFile = activeFile_ >= 0;
  MenuBar bar;

  // Settings: in the application menu on macOS, a menu of its own elsewhere.
  std::vector<MenuItem> langs;
  for (auto& l : languages()) {
    MenuItem m = item(l.name, "", [this, code = l.code] {
      setLanguage(code);
      saveSettings();
      if (nativeMenu_) nativeMenuRefresh();
    }, true, currentLanguage() == l.code);
    if (l.total > 0 && l.translated < l.total) m.hint = std::to_string(100 * l.translated / l.total) + " %";
    langs.push_back(m);
  }
  std::vector<MenuItem> settings = {item(native ? T("Tools...") : T("Tools"), native ? "Ctrl+," : "", [this] { showTools_ = true; }),
                                    sub(T("Language"), langs)};
  if (native) bar.push_back(sub("VCSIDE", settings));

  // File
  std::vector<MenuItem> recent;
  for (auto& r : recent_) recent.push_back(item(r, "", [this, r] { openPath(r); }));
  std::vector<MenuItem> file = {
      item(T("New project..."), "", [this] { showNewProject_ = true; }),
      item(T("Open project..."), "", [this] {
        static const SDL_DialogFileFilter filters[] = {{"VCSIDE project", "vcsproj"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "project"}, window_, filters, 2, nullptr, false);
      }),
      item(T("Open folder..."), "", [this] { SDL_ShowOpenFolderDialog(dialogCallback, new DialogCtx{this, "folder"}, window_, nullptr, false); }),
      sub(T("Recent projects"), recent, !recent.empty()),
      item(T("New graphics..."), "", [this] {
        static const SDL_DialogFileFilter filters[] = {{"VCSIDE graphics", "vcsgfx"}};
        std::string where = (project_ ? fs::path(project_->dir) : fs::path(folder_.empty() ? "." : folder_)) / "graphics.vcsgfx";
        SDL_ShowSaveFileDialog(dialogCallback, new DialogCtx{this, "newgfx"}, window_, filters, 1, where.c_str());
      }),
      MenuItem::sep(),
      item(T("Save"), "Ctrl+S", [this] { if (activeFile_ >= 0) saveFile(*files_[activeFile_]); }, hasFile),
      item(T("Save all"), "Ctrl+Shift+S", [this] { saveAll(); }),
      item(T("Close file"), "", [this] { if (activeFile_ >= 0) closeRequest_ = activeFile_; }, hasFile)};
  if (!native) {
    file.push_back(MenuItem::sep());
    file.push_back(item(T("Quit"), "", [this] { requestQuit(); }));
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
        else if (TextEditor* e = editor()) e->undo();
      }),
      item(T("Redo"), "Ctrl+Shift+Z", [this, editor] {
        if (activeFile_ >= 0 && files_[activeFile_]->gfx) files_[activeFile_]->gfx->redo();
        else if (TextEditor* e = editor()) e->redo();
      }),
      MenuItem::sep(),
      item(T("Cut"), "Ctrl+X", onEditor([](TextEditor& e) { e.cut(); })),
      item(T("Copy"), "Ctrl+C", onEditor([](TextEditor& e) { e.copy(); })),
      item(T("Paste"), "Ctrl+V", onEditor([](TextEditor& e) { e.paste(); })),
      item(T("Select all"), "Ctrl+A", onEditor([](TextEditor& e) { e.selectAll(); })),
      MenuItem::sep(),
      item(T("Find..."), "Ctrl+F", onEditor([](TextEditor& e) { e.openFind(false); })),
      item(T("Replace..."), replaceKey, onEditor([](TextEditor& e) { e.openFind(true); })),
      item(T("Find next"), "F3", onEditor([](TextEditor& e) { e.findNext(false); })),
      item(T("Find previous"), "Shift+F3", onEditor([](TextEditor& e) { e.findNext(true); })),
      item(T("Go to line..."), "Ctrl+G", onEditor([](TextEditor& e) { e.openGoto(); })),
      item(T("Go to address..."), "Ctrl+Shift+G", [this] { showGoToAddress_ = true; }, !addresses_.empty()),
      item(T("Go to definition"), "F12", [this] {
        if (activeFile_ < 0) return;
        auto& f = *files_[activeFile_];
        gotoDefinition(f.editor.wordAtCursor(), f.path, f.editor.cursor().line);
      }, asmFile),
      MenuItem::sep(),
      item(T("Toggle comment"), "Ctrl+/", onEditor([](TextEditor& e) { e.toggleComment(); })),
      item(T("Duplicate lines"), "Ctrl+D", onEditor([](TextEditor& e) { e.duplicateLines(); })),
      item(T("Move lines up"), "Alt+Up", onEditor([](TextEditor& e) { e.moveLines(-1); })),
      item(T("Move lines down"), "Alt+Down", onEditor([](TextEditor& e) { e.moveLines(1); })),
      item(T("Delete lines"), "Ctrl+Shift+K", onEditor([](TextEditor& e) { e.deleteLines(); })),
  }, hasFile));

  // Build
  bool p = project_.has_value() && !building_;
  std::vector<MenuItem> b = {item(T("Build"), "F7", [this] { build(0); }, p),
                             item(T("Build and run"), "F5", [this] { build(1); }, p),
                             item(T("Build and debug"), "F6", [this] { build(2); }, p),
                             item(T("Run"), "", [this] { run(false); }, p),
                             MenuItem::sep(),
                             item(T("Toggle breakpoint"), "F9", onEditor([](TextEditor& e) { e.toggleBreakpoint(e.cursor().line); }), hasFile),
                             item(T("Clear all breakpoints"), "", [this] {
                               for (auto& f : files_) f->editor.setBreakpoints({});
                               breakpoints_.clear();
                               saveUserState();
                             })};
  if (project_) {
    auto set = [this](std::function<void(Project&)> f) {
      return [this, f] {
        if (!project_) return;
        f(*project_);
        std::string err;
        project_->save(&err);
        for (auto& file : files_) lint(*file);
      };
    };
    std::vector<MenuItem> formats, tvs, emus, illegal;
    // AUTO, then a submenu per category with its formats in name order (8K before 16K)
    formats.push_back(item(T("Detected by the emulator (AUTO)"), "", set([](Project& pr) { pr.format = "AUTO"; }), true,
                           project_->format == "AUTO"));
    formats.push_back(MenuItem::sep());
    for (auto& [cat, title] : formatCategories()) {
      std::vector<const CartFormat*> in;
      for (auto& f : kb_.formats())
        if (f.category == cat) in.push_back(&f);
      std::sort(in.begin(), in.end(), [](auto* a, auto* b) { return naturalLess(a->name, b->name); });
      std::vector<MenuItem> items;
      bool current = false;
      for (auto* f : in) {
        MenuItem m = item(f->id, "", set([id = f->id](Project& pr) { pr.format = id; }), true, project_->format == f->id);
        if (!native) m.hint = f->name;  // on the right
        else m.label = f->name.find(f->id) != std::string::npos ? f->name : f->id + " — " + f->name;
        current = current || m.checked;
        items.push_back(m);
      }
      if (items.empty()) continue;
      MenuItem s = sub(title, items);
      s.checked = current;  // marks the category of the project's format
      formats.push_back(s);
    }
    for (auto& tv : tvStandardIds()) {
      MenuItem m = item(tv == "AUTO" ? T("Chosen by the emulator") : tv, "", set([tv](Project& pr) { pr.tv = tv; }), true, project_->tv == tv);
      if (!tvSupported(project_->emulator, tv)) {
        Tool* t = tools_.get(project_->emulator);
        m.hint = F("not in {0}", {t ? t->name : project_->emulator});
      }
      tvs.push_back(m);
    }
    for (auto e : {"stella", "gopher2600"}) {
      Tool* t = tools_.get(e);
      MenuItem m = item(t->name, "", set([e](Project& pr) { pr.emulator = e; }), true, project_->emulator == e);
      m.hint = t->found() ? T("found") : T("not found");
      emus.push_back(m);
    }
    const IllegalOpcodes vals[] = {IllegalOpcodes::Allow, IllegalOpcodes::StableOnly, IllegalOpcodes::Warn, IllegalOpcodes::Forbid};
    const char* names[] = {N_("Allow"), N_("Stable only"), N_("Warn"), N_("Forbid")};
    for (int i = 0; i < 4; i++)
      illegal.push_back(item(T(names[i]), "", set([v = vals[i]](Project& pr) { pr.illegal = v; }), true, project_->illegal == vals[i]));
    b.push_back(MenuItem::sep());
    b.push_back(sub(T("Format"), formats));
    b.push_back(sub(T("TV standard"), tvs));
    b.push_back(sub(T("Emulator"), emus));
    b.push_back(item(T("Emulator options..."), "", [this] { showEmuOptions_ = true; }));
    b.push_back(sub(T("Illegal opcodes"), illegal));
  }
  bar.push_back(sub(T("Build"), b));

  // View
  std::vector<MenuItem> themeMenu;
  for (int pass = 0; pass < 2; pass++) {
    bool first = true;
    for (auto& t : themes_.themes())
      if (t.builtIn == (pass == 0)) {
        if (pass == 1 && first) themeMenu.push_back(MenuItem::sep());  // the user's themes after the built-in ones
        first = false;
        themeMenu.push_back(item(theme::displayName(t), "", [this, id = t.id] { useTheme(id); }, true, t.id == themeId_));
      }
  }
  themeMenu.push_back(MenuItem::sep());
  themeMenu.push_back(item(T("Edit themes..."), "", [this] { themeEditor_.open = true; }));
  bar.push_back(sub(T("View"), {
      item(T("Show cycle column"), "", [this] {
        showCycles_ = !showCycles_;
        for (auto& f : files_) f->editor.setGutterWidthChars(showCycles_ && f->asmSyntax ? 5 : 0);
        saveSettings();
      }, true, showCycles_),
      MenuItem::sep(),
      item(T("Zoom in"), "Ctrl++", [this] { zoomEditor(1); }, zoom_ < 3.0f),
      item(T("Zoom out"), "Ctrl+-", [this] { zoomEditor(-1); }, zoom_ > 0.5f),
      item(F("Actual size ({0} %)", {std::to_string((int)(zoom_ * 100 + 0.5f))}), "Ctrl+0", [this] { zoomEditor(0); },
           zoom_ != 1.0f),
      MenuItem::sep(),
      sub(T("Theme"), themeMenu),
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
    for (auto& a : nativeMenuTakeActions()) a();
    return;
  }
  if (!ImGui::BeginMainMenuBar()) return;
  drawImGuiMenus(buildMenuBar(false));
  // status on the right of the menu bar
  if (!lastStatus_.empty()) {
    float w = ImGui::CalcTextSize(lastStatus_.c_str()).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - 20);
    ImGui::TextDisabled("%s", lastStatus_.c_str());
  }
  ImGui::EndMainMenuBar();
}

void App::dockspace() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  // leave room for the status bar
  float statusH = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusH));
  ImGui::SetNextWindowViewport(vp->ID);
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
  ImGuiViewport* vp = ImGui::GetMainViewport();
  float h = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
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
    ImGui::Text("%s  |  %s  %s  %s", project_->name.c_str(), project_->format.c_str(), project_->tv.c_str(),
                project_->emulator.c_str());
    ImGui::SameLine();
  }
  if (activeFile_ >= 0) {
    auto& f = *files_[activeFile_];
    auto& ed = f.editor;
    ImGui::Text("|  %s %d, %s %d", T("Line"), ed.cursor().line + 1, T("Col"), ed.cursor().col + 1);
    if (f.asmSyntax && ed.hasSelection()) {
      auto [a, b] = ed.selectedLines();
      std::vector<std::string> sel(ed.lines().begin() + a, ed.lines().begin() + b + 1);
      BlockCycles c = countBlock(kb_, sel, [this](const std::string& s) { return lookupSymbol(s); });
      ImGui::SameLine();
      std::string cyc = std::to_string(c.min) + (c.max != c.min ? "-" + std::to_string(c.max) : "");
      std::string buf = "|  " + F("{0} lines: {1} cycles, {2} bytes", {std::to_string(c.lines), cyc, std::to_string(c.bytes)});
      ImGui::TextColored(theme::colv(theme::Role::MsgHighlight), "%s", buf.c_str());
      ImGui::SameLine();
      char sl[48];
      snprintf(sl, sizeof sl, "%.2f-%.2f", c.min / 76.0, c.max / 76.0);
      std::string extra = F("({0} scanlines)", {sl});
      if (c.unknown) extra += "  " + F("{0} unknown", {std::to_string(c.unknown)});
      if (c.guessed) extra += "  " + F("{0} guessed (~)", {std::to_string(c.guessed)});
      ImGui::TextDisabled("%s", extra.c_str());
    }
  }
  if (space_ || elfSpace_) {
    std::string sum = space_ ? space_->summary() : elfSpace_->summary();
    bool err = false, warn = false;
    for (auto& d : space_ ? space_->diagnostics : elfSpace_->diagnostics)
      err |= d.severity == Diagnostic::Error, warn |= d.severity == Diagnostic::Warning;
    float w = ImGui::CalcTextSize(sum.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, ImGui::GetWindowWidth() - w - 12));
    ImVec4 col = theme::colv(err ? theme::Role::MsgError : warn ? theme::Role::MsgWarning : theme::Role::UiTextDim);
    ImGui::TextColored(col, "%s", sum.c_str());
  }
  ImGui::End();
}

void App::projectPanel() {
  char title[64];
  snprintf(title, sizeof title, "%s###project", T("Project"));
  if (!ImGui::Begin(title)) {
    ImGui::End();
    return;
  }
  if (folder_.empty()) {
    ImGui::TextWrapped("%s", T("No project open."));
    if (ImGui::Button(T("New project..."))) showNewProject_ = true;
    if (ImGui::Button(T("Open project..."))) {
      static const SDL_DialogFileFilter filters[] = {{"VCSIDE project", "vcsproj"}};
      SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "project"}, window_, filters, 1, nullptr, false);
    }
    for (auto& r : recent_)
      if (ImGui::Selectable(fs::path(r).filename().string().c_str())) openPath(r);
    ImGui::End();
    return;
  }
  // The tree, drawn row by row: chevron, icon, name; guides for each level.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float fontH = ImGui::GetTextLineHeight(), rowH = std::round(fontH * 1.45f), indentW = std::round(fontH * 0.9f);
  float iconS = std::round(fontH * 0.95f);
  float left = ImGui::GetWindowPos().x, right = left + ImGui::GetWindowWidth();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().WindowPadding.y * 0.5f);
  auto isOpen = [&](const std::string& p, bool byDefault) { return treeToggled_.count(p) ? !byDefault : byDefault; };
  auto toggle = [&](const std::string& p) {
    if (!treeToggled_.erase(p)) treeToggled_.insert(p);
  };
  // One row; true if clicked. `depth` 0 is the project's own row.
  auto row = [&](const std::string& id, int depth, const std::string& name, files::Kind kind, bool dir, bool open,
                 bool active, bool dim) {
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushID(id.c_str());
    bool clicked = ImGui::InvisibleButton("row", ImVec2(std::max(1.0f, right - at.x), rowH));
    bool hover = ImGui::IsItemHovered();
    ImGui::PopID();
    if (active) dl->AddRectFilled(ImVec2(left, at.y), ImVec2(right, at.y + rowH), theme::col(theme::Role::FilesRowSelected));
    else if (hover) dl->AddRectFilled(ImVec2(left, at.y), ImVec2(right, at.y + rowH), theme::col(theme::Role::FilesRowHover));
    float x = at.x + depth * indentW, cy = at.y + rowH * 0.5f;
    for (int d = 1; d <= depth; d++) {  // a guide under each open folder above
      float gx = std::round(at.x + (d - 1) * indentW + indentW * 0.5f) + 0.5f;
      dl->AddLine(ImVec2(gx, at.y), ImVec2(gx, at.y + rowH), theme::col(theme::Role::FilesGuide));
    }
    ImU32 text = theme::col(dim ? theme::Role::UiTextDim : theme::Role::UiText);
    if (dir) {  // chevron: > closed, v open
      float c = fontH * 0.22f, cx = x + indentW * 0.5f, t = std::max(1.0f, fontH * 0.09f);
      ImU32 cc = theme::col(theme::Role::UiTextDim);
      if (open) dl->AddPolyline(std::array<ImVec2, 3>{ImVec2(cx - c, cy - c * 0.5f), ImVec2(cx, cy + c * 0.5f), ImVec2(cx + c, cy - c * 0.5f)}.data(), 3, cc, 0, t);
      else dl->AddPolyline(std::array<ImVec2, 3>{ImVec2(cx - c * 0.5f, cy - c), ImVec2(cx + c * 0.5f, cy), ImVec2(cx - c * 0.5f, cy + c)}.data(), 3, cc, 0, t);
    }
    x += indentW;
    if (depth > 0 || !dir) {
      files::drawIcon(dl, ImVec2(x, std::round(cy - iconS * 0.5f)), iconS, kind, open);
      x += iconS + fontH * 0.4f;
    }
    dl->AddText(ImVec2(x, std::round(cy - fontH * 0.5f)), text, name.c_str());
    if (hover && ImGui::CalcTextSize(name.c_str()).x > right - x) ImGui::SetTooltip("%s", name.c_str());
    return clicked;
  };
  std::function<void(const fs::path&, int, bool)> tree = [&](const fs::path& dir, int depth, bool dim) {
    std::vector<fs::directory_entry> entries;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
      std::string n = e.path().filename().string();
      if (n.empty() || n[0] == '.') continue;
      entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
      if (a.is_directory() != b.is_directory()) return a.is_directory();
      return lower(a.path().filename().string()) < lower(b.path().filename().string());
    });
    for (auto& e : entries) {
      std::string n = e.path().filename().string(), p = e.path().string();
      bool d = e.is_directory();
      if (d) {
        bool sub = dim || n == "build";  // build output: shown dimmed
        bool open = isOpen(p, false);
        if (row(p, depth, n, files::Kind::Folder, true, open, false, sub)) toggle(p), open = !open;
        if (open) tree(e.path(), depth + 1, sub);
      } else {
        bool active = activeFile_ >= 0 && files_[activeFile_]->path == p;
        if (row(p, depth, n, files::kindOf(n, false), false, false, active, dim)) {
          if (e.path().extension() == ".vcsproj") openProject(p);
          else if (files::kindOf(n, false) != files::Kind::Rom) openFile(p);
        }
      }
    }
  };
  // the project's row: its name in capitals, like a section title
  std::string rootName = fs::path(folder_).filename().string();
  for (auto& c : rootName) c = (char)std::toupper((unsigned char)c);
  bool rootOpen = isOpen(folder_, true);
  if (row(folder_, 0, rootName, files::Kind::Folder, true, rootOpen, false, false)) toggle(folder_), rootOpen = !rootOpen;
  if (rootOpen) tree(folder_, 1, false);
  ImGui::PopStyleVar();
  ImGui::End();
}

void App::editorsPanel() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  bool open = ImGui::Begin("Editor###editors", nullptr, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  if (!open) {
    ImGui::End();
    return;
  }
  if (files_.empty()) {
    ImGui::SetCursorPos(ImVec2(20, 20));
    ImGui::TextDisabled("VCSIDE %s", VCSIDE_VERSION);
    ImGui::SetCursorPosX(20);
    ImGui::TextDisabled("F7 %s   F5 %s   F6 %s   F1 %s", T("Build"), T("Build and run"), T("Build and debug"), T("Documentation"));
    ImGui::End();
    return;
  }
  if (ImGui::BeginTabBar("files", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
    for (int i = 0; i < (int)files_.size(); i++) {
      auto& f = *files_[i];
      bool keep = true;
      std::string label = fs::path(f.path).filename().string() + (f.dirty() ? " *" : "") + "###" + f.path;
      ImGuiTabItemFlags fl = f.requestFocus ? ImGuiTabItemFlags_SetSelected : 0;
      if (f.dirty()) fl |= ImGuiTabItemFlags_UnsavedDocument;
      if (ImGui::BeginTabItem(label.c_str(), &keep, fl)) {
        activeFile_ = i;
        if (f.requestFocus) {
          ImGui::SetKeyboardFocusHere();
          f.requestFocus = false;
        }
        if (f.gfx) {
          ImGui::BeginChild("##gfx", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar);
          f.gfx->render(gfxTv(), gfxProfile());
          ImGui::EndChild();
        } else if (f.editor.render("##code", editorFont())) {
          lint(f);
        }
        if (int z = f.editor.takeZoomSteps()) zoomEditor(z);
        if (f.editor.takeBreakpointsChanged()) {
          breakpoints_[f.path] = f.editor.breakpoints();
          saveUserState();
        }
        ImGui::EndTabItem();
      }
      if (!keep) closeRequest_ = i;
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
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
  if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(T("Save changes before closing?"));
    if (closeRequest_ >= 0) ImGui::TextDisabled("%s", files_[closeRequest_]->path.c_str());
    int action = 0;
    if (ImGui::Button(T("Save"))) action = 1;
    ImGui::SameLine();
    if (ImGui::Button(T("Don't save"))) action = 2;
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"))) action = 3;
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
  if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    quitConfirm_ = false;
    ImGui::TextUnformatted(T("Save changes before closing?"));
    if (ImGui::Button(T("Save all"))) saveAll(), quit_ = true, ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button(T("Don't save"))) quit_ = true, ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void App::outputPanel() {
  char title[64];
  snprintf(title, sizeof title, "%s###output", T("Output"));
  bool open = ImGui::Begin(title);
  bottomDock_ = ImGui::GetWindowDockID();
  if (open) {
    ImGui::PushFont(mono_);
    ImGui::TextUnformatted(output_.c_str());
    if (outputScroll_ || building_) ImGui::SetScrollHereY(1.0f);
    outputScroll_ = false;
    ImGui::PopFont();
  }
  ImGui::End();
}

void App::problemsPanel() {
  char title[64];
  snprintf(title, sizeof title, "%s (%d)###problems", T("Problems"), (int)diagnostics_.size());
  if (ImGui::Begin(title)) {
    if (ImGui::BeginTable("diag", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
      ImGui::TableSetupColumn(T("File"), ImGuiTableColumnFlags_WidthFixed, 200);
      ImGui::TableSetupColumn("");
      for (size_t i = 0; i < diagnostics_.size(); i++) {
        auto& d = diagnostics_[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImVec4 col = theme::colv(d.severity == Diagnostic::Error ? theme::Role::MsgError : d.severity == Diagnostic::Warning ? theme::Role::MsgWarning : theme::Role::MsgInfo);
        ImGui::PushID((int)i);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        bool clicked = ImGui::Selectable(d.severity == Diagnostic::Error ? T("error") : d.severity == Diagnostic::Warning ? T("warning") : T("info"), false,
                                         ImGuiSelectableFlags_SpanAllColumns);
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::TableNextColumn();
        if (!d.file.empty()) {
          std::string shown = d.file;
          std::error_code ec;
          if (project_ && fs::path(d.file).is_absolute()) {
            auto rel = fs::relative(d.file, project_->dir, ec);
            if (!ec && !rel.empty() && rel.native()[0] != '.') shown = rel.string();
          }
          if (d.line) ImGui::Text("%s:%d", shown.c_str(), d.line);
          else ImGui::TextUnformatted(shown.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(d.message.c_str());
        if (clicked && !d.file.empty() && project_) {
          std::string p = findProjectFile(d.file);
          if (!p.empty()) openFile(p, std::max(1, d.line));
        }
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
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

static void wrappedTooltip(const std::string& text) {
  if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

void App::emulatorOptionsDialog() {
  if (showEmuOptions_) {
    ImGui::OpenPopup("###emuopts");
    showEmuOptions_ = false;
    emuOptionsTab_ = project_ ? project_->emulator : "";
  }
  char title[96];
  snprintf(title, sizeof title, "%s###emuopts", T("Emulator options"));
  float fs = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(fs * 50, fs * 42), ImGuiCond_Appearing);
  bool open = true;
  if (!ImGui::BeginPopupModal(title, &open)) return;
  if (!project_) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  bool changed = false;
  if (ImGui::BeginTabBar("emus")) {
    for (const char* id : {"stella", "gopher2600"}) {
      const EmulatorInfo* emu = kb_.emulator(id);
      if (!emu) continue;
      ImGuiTabItemFlags tabFlags = emuOptionsTab_ == id ? ImGuiTabItemFlags_SetSelected : 0;
      if (!ImGui::BeginTabItem(emu->name.c_str(), nullptr, tabFlags)) continue;
      auto& set = project_->emulatorOptions[id];
      ImGui::BeginChild("opts", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 6), ImGuiChildFlags_Borders);
      // groups in the order they first appear, each with all its options
      std::vector<const EmuOption*> ordered;
      for (auto& g : emu->options)
        if (std::none_of(ordered.begin(), ordered.end(), [&](const EmuOption* x) { return x->group == g.group; }))
          for (auto& o : emu->options)
            if (o.group == g.group) ordered.push_back(&o);
      std::string group;
      bool table = false;
      for (const EmuOption* op : ordered) {
        const EmuOption& o = *op;
        if (o.group != group || !table) {
          if (table) ImGui::EndTable();
          group = o.group;
          ImGui::SeparatorText(T(group.c_str()));
          table = ImGui::BeginTable(group.c_str(), 2, ImGuiTableFlags_SizingStretchProp);
          if (!table) continue;
          ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 3);
          ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 2);
        }
        ImGui::PushID(o.id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        auto cur = set.find(o.id);
        std::string value = cur == set.end() ? "" : cur->second;
        if (value.empty()) ImGui::TextUnformatted(o.labelText().c_str());
        else ImGui::TextColored(theme::colv(theme::Role::MsgInfo), "%s", o.labelText().c_str());
        std::string tip = o.descriptionText() + "\n\n";
        if (!o.arg.empty()) tip += o.arg + (o.noValue ? "" : " <" + o.type + ">");
        else if (!o.flag.empty()) tip += o.flag;
        else tip += F("Preference {0} (VCSIDE passes it through a copy of the preferences file)", {o.pref});
        if (o.developer) tip += "\n" + std::string(T("Needs Stella's developer settings: VCSIDE turns them on."));
        wrappedTooltip(tip);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        std::string next = value;
        if (o.type == "int" || o.type == "text") {
          std::string hint = T("default");
          if (o.max > 0) hint += " (" + std::to_string(o.min) + "-" + std::to_string(o.max) + ")";
          ImGuiInputTextFlags fl = o.type == "int" ? ImGuiInputTextFlags_CharsDecimal : 0;
          ImGui::InputTextWithHint("##v", hint.c_str(), &next, fl);
          if (o.type == "int" && !next.empty() && o.max > 0) {
            int n = std::atoi(next.c_str());
            if (n < o.min || n > o.max) ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", T("out of range"));
          }
        } else {
          std::string preview = T("default");
          for (auto& v : o.values)
            if (v.value == value) preview = T(v.label.c_str());
          if (ImGui::BeginCombo("##v", preview.c_str())) {
            if (ImGui::Selectable(T("default"), value.empty())) next.clear();
            for (auto& v : o.values)
              if (ImGui::Selectable(T(v.label.c_str()), v.value == value)) next = v.value;
            ImGui::EndCombo();
          }
        }
        wrappedTooltip(tip);
        if (next != value) {
          if (next.empty()) set.erase(o.id);
          else set[o.id] = next;
          changed = true;
        }
        ImGui::PopID();
      }
      if (table) ImGui::EndTable();
      ImGui::EndChild();

      std::string& extra = project_->emulatorArgs[id];
      ImGui::SetNextItemWidth(-fs * 12);
      if (ImGui::InputText(T("Extra arguments"), &extra)) changed = true;
      std::map<std::string, std::string> prefs;
      auto args = emulatorOptionArgs(*project_, *emu, &prefs);
      for (auto& a : splitCommand(extra)) args.push_back(a);
      std::string line = args.empty() ? T("(nothing)") : quoteArgs(args);
      ImGui::TextDisabled("%s", T("Added to the command line:"));
      ImGui::SameLine();
      ImGui::TextWrapped("%s", line.c_str());
      if (!prefs.empty()) {
        std::string p;
        for (auto& [k, v] : prefs) p += (p.empty() ? "" : ", ") + k + " = " + v;
        ImGui::TextDisabled("%s", T("Preferences:"));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", p.c_str());
      }
      if (ImGui::Button(T("Reset to defaults"))) {
        set.clear();
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
  if (ImGui::Button(T("Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  if (changed) {
    std::string err;
    if (!project_->save(&err)) lastStatus_ = err;
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
  if (!ImGui::BeginPopupModal(T("Go to address"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextWrapped("%s", T("Address the CPU sees ($F1A0, F1A0 or a label), optionally followed by the bank: $F1A0 1"));
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  bool enter = ImGui::InputText("##addr", goToAddress_, sizeof goToAddress_, ImGuiInputTextFlags_EnterReturnsTrue);
  // parse "addr [bank]"
  std::istringstream in(goToAddress_);
  std::string a, b;
  in >> a >> b;
  std::optional<int> addr;
  if (!a.empty()) {
    std::string e = a;
    if (e.size() > 2 && (e[0] == '0') && (e[1] == 'x' || e[1] == 'X')) e = "$" + e.substr(2);
    else if (e[0] != '$' && std::all_of(e.begin(), e.end(), [](char c) { return std::isxdigit((unsigned char)c); })) e = "$" + e;
    addr = evalExpr(e, [this](const std::string& s) { return lookupSymbol(s); });
    if (!addr) addr = evalExpr(a, [this](const std::string& s) { return lookupSymbol(s); });
  }
  int bank = b.empty() ? -1 : std::atoi(b.c_str());
  std::vector<const ListedLine*> hits;
  if (addr) hits = addresses_.linesAt(*addr, bank);
  bool go = false;
  const ListedLine* target = nullptr;
  if (!a.empty() && hits.empty()) ImGui::TextDisabled("%s", T("No code at that address in the last build."));
  for (size_t i = 0; i < hits.size() && i < 12; i++) {
    std::string label = fs::path(hits[i]->file).filename().string() + ":" + std::to_string(hits[i]->line);
    if (addresses_.banks() > 1) label += "  " + F("(bank {0})", {std::to_string(addresses_.bankOf(*hits[i]))});
    if (ImGui::Selectable(label.c_str(), i == 0)) target = hits[i], go = true;
  }
  if (enter && !hits.empty()) target = hits[0], go = true;
  if (go && target) {
    openFile(target->file, target->line);
    ImGui::CloseCurrentPopup();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

static void spaceBar(const SpaceReport::Area& a, int warnPercent) {
  ImGui::TextUnformatted(a.name.c_str());
  ImGui::SameLine(ImGui::GetFontSize() * 9);
  if (!a.measured) {
    ImGui::TextDisabled("%s", a.note.c_str());
    return;
  }
  float frac = a.size > 0 ? (float)a.used / (float)a.size : 0;
  int pct = a.size > 0 ? (int)((a.used * 100 + a.size - 1) / a.size) : 0;
  ImVec4 col = theme::colv(a.used > a.size ? theme::Role::SpaceFull : pct >= warnPercent ? theme::Role::SpaceWarning : theme::Role::SpaceOk);
  std::string label = F("{0} / {1} bytes ({2} %), {3} free",
                        {std::to_string(a.used), std::to_string(a.size), std::to_string(pct), std::to_string(std::max(0L, a.size - a.used))});
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
  ImGui::ProgressBar(std::min(frac, 1.0f), ImVec2(ImGui::GetFontSize() * 24, 0), label.c_str());
  ImGui::PopStyleColor();
}

void App::spacePanel() {
  char title[64];
  snprintf(title, sizeof title, "%s###space", T("Space"));
  if (bottomDock_) ImGui::SetNextWindowDockID(bottomDock_, ImGuiCond_FirstUseEver);
  if (ImGui::Begin(title)) {
    if (spaceRunning_) ImGui::TextDisabled("%s", T("Measuring..."));
    if (!spaceNote_.empty()) ImGui::TextWrapped("%s", spaceNote_.c_str());
    if (elfSpace_) {
      int warn = project_ ? project_->spaceWarnPercent : 90;
      auto& e = *elfSpace_;
      ImGui::SeparatorText(F("ROM ({0} bytes)", {std::to_string(e.fileSize)}).c_str());
      const std::pair<const char*, long> parts[] = {{N_("Code"), e.code}, {N_("Code in RAM (.RamFunc)"), e.ramCode},
                                                    {N_("Constants"), e.constants}, {N_("Variables with a value"), e.data},
                                                    {N_("Zeroed variables"), e.bss}};
      if (ImGui::BeginTable("elfparts", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (auto& [name, n] : parts) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(T(name));
          ImGui::TableNextColumn();
          ImGui::Text("%s", F("{0} bytes", {std::to_string(n)}).c_str());
        }
        ImGui::EndTable();
      }
      ImGui::SeparatorText(T("Chameleon Cart (STM32G070RB)"));
      SpaceReport::Area fl{e.flash.name, e.flash.used, e.flash.size, true, ""}, ra{e.ram.name, e.ram.used, e.ram.size, true, ""};
      spaceBar(fl, warn);
      spaceBar(ra, warn);
      ImGui::PushTextWrapPos(0);
      if (e.exact)
        ImGui::TextDisabled("%s", T("Measured on the Chameleon image (make chameleon), firmware included."));
      else
        ImGui::TextDisabled("%s", F("Estimate: the game plus about {0} bytes of flash and {1} of RAM for the firmware. Build the Chameleon image (make chameleon) for the exact figures.",
                                    {std::to_string(kChameleonFirmwareFlash), std::to_string(kChameleonFirmwareRam)})
                                      .c_str());
      ImGui::TextDisabled("%s", T("The 512 KB Chameleon Cart (STM32G0B0RE) has 144 KB of RAM. Stella, Gopher2600 and the PlusCart load the whole ROM into RAM."));
      ImGui::PopTextWrapPos();
    } else if (!space_) {
      if (!spaceRunning_ && spaceNote_.empty()) ImGui::TextDisabled("%s", T("Build the project (F7) to see the space used."));
    } else {
      int warn = project_ ? project_->spaceWarnPercent : 90;
      ImGui::SeparatorText(F("ROM ({0} bytes)", {std::to_string(space_->romFileSize)}).c_str());
      for (auto& b : space_->banks) spaceBar(b, warn);
      ImGui::SeparatorText(T("RAM"));
      spaceBar(space_->ram, warn);
      if (space_->ram.measured)
        ImGui::TextDisabled("%s", F("{0} bytes above the variables for the stack (reserve: {1}).",
                                     {std::to_string(space_->stackRoom), std::to_string(project_ ? project_->stackReserve : 8)})
                                      .c_str());
      if (space_->cartRam) spaceBar(*space_->cartRam, warn);
    }
  }
  ImGui::End();
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
    const Source* src = kb.source(id);
    ImGui::Bullet();
    if (!src) {
      ImGui::TextDisabled("%s", id.c_str());
      continue;
    }
    std::string label = src->title + (src->author.empty() ? "" : " (" + src->author + ")");
    if (src->url.rfind("http", 0) == 0) ImGui::TextLinkOpenURL(label.c_str(), src->url.c_str());
    else ImGui::TextWrapped("%s", label.c_str());
  }
}

static void bullet(const std::string& s) {
  ImGui::Bullet();
  ImGui::TextWrapped("%s", s.c_str());
}

void App::docsPanel() {
  char title[64];
  snprintf(title, sizeof title, "%s###docs", T("Documentation"));
  if (focusDocs_) ImGui::SetNextWindowFocus();
  if (!ImGui::Begin(title)) {
    ImGui::End();
    return;
  }
  if (focusDocs_ && !docTopic_.empty()) docQuery_ = docTopic_;
  focusDocs_ = false;
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##q", T("Search"), &docQuery_);
  std::string q = upper(docQuery_);
  auto match = [&](const std::string& s) { return q.empty() || upper(s).find(q) != std::string::npos; };

  ImGui::BeginChild("list", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.35f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
  auto item = [&](const std::string& id, const std::string& label, ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    if (ImGui::Selectable((label + "##" + id).c_str(), docTopic_ == id)) docTopic_ = id;
    ImGui::PopStyleColor();
  };
  const ImVec4 legal = theme::colv(theme::Role::SynMnemonic), illegal = theme::colv(theme::Role::SynIllegal),
               unstable = theme::colv(theme::Role::SynUnstable), plain = theme::colv(theme::Role::UiText);
  ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (ImGui::TreeNode(T("Instructions"))) {
    const char* groups[] = {N_("Legal"), N_("Illegal (stable)"), N_("Illegal (unstable)")};
    for (int g = 0; g < 3; g++) {
      if (ImGui::TreeNodeEx(T(groups[g]), g == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        for (auto& [mn, in] : kb_.instructions()) {
          bool stable = in.opcodes.empty() || in.opcodes[0]->stable;
          int grp = !in.illegal ? 0 : stable ? 1 : 2;
          if (grp != g || !(match(mn) || match(in.summary))) continue;
          item("I:" + mn, mn + "  " + in.summary, g == 0 ? legal : g == 1 ? illegal : unstable);
        }
        ImGui::TreePop();
      }
    }
    ImGui::TreePop();
  }
  if (ImGui::TreeNode(T("Registers"))) {
    for (auto& r : kb_.registers())
      if (match(r.name) || match(r.summary)) item("R:" + r.name, r.name + "  " + r.address + "  " + r.summary, plain);
    ImGui::TreePop();
  }
  if (ImGui::TreeNode(T("Cartridge formats"))) {
    for (auto& [cat, title] : formatCategories()) {
      std::vector<const CartFormat*> in;
      for (auto& f : kb_.formats())
        if (f.category == cat && (match(f.id) || match(f.name))) in.push_back(&f);
      if (in.empty()) continue;
      std::sort(in.begin(), in.end(), [](auto* a, auto* b) { return naturalLess(a->name, b->name); });
      ImGui::SeparatorText(title.c_str());
      for (auto* f : in) item("F:" + f->id, f->id + "  " + f->name, plain);
    }
    ImGui::TreePop();
  }
  if (ImGui::TreeNode(T("TV standards"))) {
    for (auto& t : kb_.tvStandards()) item("T:" + t.id, t.id, plain);
    ImGui::TreePop();
  }
  ImGui::EndChild();

  ImGui::BeginChild("topic");
  std::string topic = docTopic_;
  // a bare word (from F1) resolves to an instruction or a register
  if (topic.find(':') == std::string::npos && !topic.empty()) {
    if (kb_.instruction(topic)) topic = "I:" + upper(topic);
    else if (kb_.reg(topic)) topic = "R:" + kb_.reg(topic)->name;
    else if (kb_.format(topic)) topic = "F:" + kb_.format(topic)->id;
    docTopic_ = topic;
  }
  if (topic.rfind("I:", 0) == 0) {
    if (const Instruction* in = kb_.instruction(topic.substr(2))) {
      bool stable = in->opcodes.empty() || in->opcodes[0]->stable;
      ImGui::PushFont(mono_);
      ImGui::TextColored(!in->illegal ? legal : stable ? illegal : unstable, "%s", in->mnemonic.c_str());
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::TextUnformatted(in->summary.c_str());
      if (in->illegal) ImGui::TextColored(stable ? illegal : unstable, "%s", stable ? T("Illegal (stable)") : T("Illegal (unstable)"));
      if (!in->flags.empty()) ImGui::Text("%s: %s", T("Flags"), in->flags.c_str());
      unverifiedNotice(in->verified);
      if (!in->aliases.empty()) {
        std::string a;
        for (auto& x : in->aliases) a += (a.empty() ? "" : ", ") + x;
        ImGui::TextDisabled("%s", F("Also known as: {0}", {a}).c_str());
      }
      ImGui::TextWrapped("%s", in->description.c_str());
      if (ImGui::BeginTable("ops", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(T("Mode"));
        ImGui::TableSetupColumn(T("Opcode"));
        ImGui::TableSetupColumn(T("Bytes"));
        ImGui::TableSetupColumn(T("Cycles"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (auto* op : in->opcodes) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::PushFont(mono_);
          std::string syn = in->mnemonic;
          auto& modes = op->mode;
          static const std::map<std::string, std::string> syntax = {
              {"imp", ""}, {"acc", " A"}, {"imm", " #$nn"}, {"zp", " $nn"}, {"zpx", " $nn,X"}, {"zpy", " $nn,Y"},
              {"abs", " $nnnn"}, {"absx", " $nnnn,X"}, {"absy", " $nnnn,Y"}, {"ind", " ($nnnn)"},
              {"indx", " ($nn,X)"}, {"indy", " ($nn),Y"}, {"rel", " label"}};
          auto it = syntax.find(modes);
          ImGui::TextUnformatted((syn + (it != syntax.end() ? it->second : "")).c_str());
          ImGui::TableNextColumn();
          ImGui::Text("$%02X", op->code);
          ImGui::TableNextColumn();
          ImGui::Text("%d", op->bytes);
          ImGui::TableNextColumn();
          if (op->branch) ImGui::Text("%d (+1/+2)", op->cycles);
          else if (op->pagePenalty) ImGui::Text("%d (+1)", op->cycles);
          else ImGui::Text("%d", op->cycles);
          ImGui::PopFont();
          ImGui::TableNextColumn();
          if (op->illegal) ImGui::TextColored(op->stable ? illegal : unstable, "%s", op->stable ? T("illegal") : T("unstable"));
          if (!op->dasm) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T(".byte only"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("DASM has no mnemonic for this opcode: write it with .byte"));
          }
        }
        ImGui::EndTable();
      }
      ImGui::TextDisabled("%s", T("(+1) page crossing; branches: +1 taken, +2 taken to another page"));
      provenance(kb_, in->sources);
    }
  } else if (topic.rfind("R:", 0) == 0) {
    if (const Register* r = kb_.reg(topic.substr(2))) {
      ImGui::PushFont(mono_);
      ImGui::TextColored(theme::colv(theme::Role::MsgName), "%s", r->name.c_str());
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::Text("%s %s  %s  bits %s", r->chip.c_str(), r->address.c_str(), r->access == "R" ? T("read") : T("write"), r->bits.c_str());
      ImGui::TextWrapped("%s", r->summary.c_str());
      unverifiedNotice(r->verified);
      ImGui::Separator();
      ImGui::TextWrapped("%s", r->description.c_str());
      if (!r->timing.empty()) {
        ImGui::SeparatorText(T("Timing"));
        ImGui::TextWrapped("%s", r->timing.c_str());
      }
      if (!r->issues.empty()) {
        ImGui::SeparatorText(T("Known issues"));
        for (auto& i : r->issues) bullet(i);
      }
      provenance(kb_, r->sources);
      if (!r->example.empty()) {
        ImGui::SeparatorText(T("Example"));
        ImGui::PushFont(mono_);
        ImGui::TextUnformatted(r->example.c_str());
        ImGui::PopFont();
      }
    }
  } else if (topic.rfind("F:", 0) == 0) {
    if (const CartFormat* f = kb_.format(topic.substr(2))) {
      ImGui::Text("%s - %s", f->id.c_str(), f->name.c_str());
      unverifiedNotice(f->verified);
      ImGui::Text("%s: %s", T("Size"), f->size.c_str());
      if (!f->ram.empty()) ImGui::TextWrapped("RAM: %s", f->ram.c_str());
      ImGui::TextWrapped("%s", f->description.c_str());
      if (!f->hotspots.empty()) {
        ImGui::SeparatorText(T("Hotspots"));
        for (auto& h : f->hotspots) bullet(h);
      }
      if (!f->detection.empty()) {
        ImGui::SeparatorText(T("Detection"));
        ImGui::TextWrapped("%s", f->detection.c_str());
      }
      ImGui::SeparatorText(T("Emulator"));
      ImGui::Text("Stella  -bs %s", f->stella.empty() ? "?" : f->stella.c_str());
      ImGui::Text("Gopher2600 -mapping %s", f->gopher2600.empty() ? "?" : f->gopher2600.c_str());
      if (!f->issues.empty()) {
        ImGui::SeparatorText(T("Known issues"));
        for (auto& i : f->issues) bullet(i);
      }
      provenance(kb_, f->sources);
    }
  } else if (topic.rfind("T:", 0) == 0) {
    if (const TvStandard* t = kb_.tv(topic.substr(2))) {
      ImGui::Text("%s", t->id.c_str());
      ImGui::TextUnformatted(F("{0} lines: VSYNC {1}, VBLANK {2}, visible {3}, overscan {4}",
                               {std::to_string(t->linesTotal), std::to_string(t->vsync), std::to_string(t->vblank),
                                std::to_string(t->visible), std::to_string(t->overscan)}).c_str());
      ImGui::Text("%s Hz", t->fps.c_str());
      unverifiedNotice(t->verified);
      ImGui::TextWrapped("%s", t->palette.c_str());
      for (auto& n : t->notes) bullet(n);
      provenance(kb_, t->sources);
    }
  } else {
    for (auto& n : kb_.cpuNotes()) bullet(n);
    for (auto& n : kb_.tiaNotes()) bullet(n);
  }
  ImGui::EndChild();
  ImGui::End();
}

void App::newProjectDialog() {
  if (showNewProject_) {
    ImGui::OpenPopup("###newproject");
    showNewProject_ = false;
    if (npDir_.empty()) {
      const char* home = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
      npDir_ = home ? home : "";
    }
  }
  char title[64];
  snprintf(title, sizeof title, "%s###newproject", T("New project..."));
  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(title, nullptr)) return;
  ImGui::InputText(T("Name"), npName_, sizeof npName_);
  ImGui::InputText(T("Folder"), &npDir_);
  ImGui::SameLine();
  if (ImGui::Button(T("Choose...")))
    SDL_ShowOpenFolderDialog(dialogCallback, new DialogCtx{this, "newdir"}, window_, npDir_.c_str(), false);
  const char* langNames[3] = {T(kLangNames[0]), T(kLangNames[1]), T(kLangNames[2])};
  ImGui::Combo(T("Language of the project"), &npLang_, langNames, 3);
  {
    std::vector<const char*> tvs;
    for (auto& tv : tvStandardIds())
      if (tv != "AUTO") tvs.push_back(tv.c_str());
    ImGui::Combo(T("TV standard"), &npTv_, tvs.data(), (int)tvs.size());
  }
  auto examples = Project::examples(languageFromId(kLangIds[npLang_]));
  npExample_ = std::clamp(npExample_, 0, (int)examples.size());
  if (!examples.empty()) {
    std::vector<const char*> names = {T("Empty project")};
    for (auto& e : examples) names.push_back(T(e.second));
    ImGui::Combo(T("Start from"), &npExample_, names.data(), (int)names.size());
  }
  fs::path target = fs::path(npDir_) / npName_;
  ImGui::TextDisabled("%s", target.string().c_str());
  static std::string error;
  if (!error.empty()) ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", error.c_str());
  if (ImGui::Button(T("Create"))) {
    Project p;
    error.clear();
    std::string example = npExample_ > 0 && npExample_ <= (int)examples.size() ? examples[npExample_ - 1].first : "";
    if (Project::create(target.string(), npName_, languageFromId(kLangIds[npLang_]), tvStandardIds()[npTv_], &p, &error, example)) {
      openProject(p.file);
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(T("Cancel"))) {
    error.clear();
    ImGui::CloseCurrentPopup();
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
  ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_Appearing);
  bool open = true;
  if (!ImGui::BeginPopupModal(title, &open)) return;
  if (ImGui::BeginTable("tools", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(T("Name"), ImGuiTableColumnFlags_WidthFixed, 180);
    ImGui::TableSetupColumn(T("Path"));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 200);
    for (auto& t : tools_.tools()) {
      ImGui::PushID(t.id.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(t.name.c_str());
      ImGui::TextDisabled("%s", T(t.role.c_str()));
      ImGui::TableNextColumn();
      if (t.found()) ImGui::TextColored(theme::colv(theme::Role::MsgOk), "%s", t.path.c_str());
      else {
        ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", T("not found"));
        ImGui::TextWrapped("%s", T(t.note.c_str()));
      }
      ImGui::TableNextColumn();
      if (ImGui::SmallButton(T("Choose...")))
        SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "tool:" + t.id}, window_, nullptr, 0, nullptr, false);
      ImGui::SameLine();
      if (ImGui::SmallButton(T("Download page"))) SDL_OpenURL(t.homepage.c_str());
      if (!t.userPath.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) t.userPath.clear(), tools_.detect(), saveSettings();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::TextDisabled("%s", Toolchain::toolsDir().c_str());
  if (ImGui::Button(T("Detect again"))) tools_.detect();
  ImGui::SameLine();
  if (ImGui::Button(T("Close"))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// A tool the project needs is missing: say what it is, how to get it, and let the user
// point at it; then carry on with what was asked (build or run).
void App::askForTool(const std::string& id, const std::string& why, std::function<void()> then) {
  Tool* t = tools_.get(id);
  if (!t) return;
  missingTool_ = id;
  missingWhy_ = why;
  missingThen_ = std::move(then);
  showMissingTool_ = true;
  std::string msg = F("{0} not found: it is needed to {1}.", {t->name, why});
  output_ += msg + "\n";
  lastStatus_ = msg;
  outputScroll_ = true;
}

void App::missingToolDialog() {
  if (showMissingTool_) {
    ImGui::OpenPopup("###missingtool");
    showMissingTool_ = false;
  }
  Tool* t = tools_.get(missingTool_);
  if (!t) return;
  char title[128];
  snprintf(title, sizeof title, "%s###missingtool", (t->found() ? F("{0} found", {t->name}) : F("{0} not found", {t->name})).c_str());
  float em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(em * 36, 0), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::PushTextWrapPos(em * 34);
  if (!t->found()) {
    ImGui::TextUnformatted(F("VCSIDE needs {0} ({1}) to {2}, and did not find it on this computer.", {t->name, T(t->role.c_str()), missingWhy_}).c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted(T(t->note.c_str()));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", F("Once installed, VCSIDE finds it in PATH, in the usual folders, or in {0}. You can also choose the file yourself.",
                                {Toolchain::toolsDir()}).c_str());
  } else {
    ImGui::TextColored(theme::colv(theme::Role::MsgOk), "%s", F("Found: {0}", {t->path}).c_str());
  }
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  bool close = false;
  if (t->found()) {
    if (ImGui::Button(T("Continue"))) {
      close = true;
      if (missingThen_) missingThen_();
    }
  } else {
    if (ImGui::Button(T("Open download page"))) SDL_OpenURL(t->homepage.c_str());
    ImGui::SameLine();
    if (ImGui::Button(T("Choose the file...")))
      SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "tool:" + t->id}, window_, nullptr, 0, nullptr, false);
    ImGui::SameLine();
    if (ImGui::Button(T("Search again"))) tools_.detect();
  }
  ImGui::SameLine();
  if (ImGui::Button(T("Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
  if (close) {
    ImGui::CloseCurrentPopup();
    missingThen_ = nullptr;
  }
  ImGui::EndPopup();
}

void App::aboutDialog() {
  if (showAbout_) {
    showAbout_ = false;
    // macOS: the system's panel; elsewhere the window below
    if (nativeMenuAvailable() && nativeShowAbout(aboutInfo(kb_, tools_))) return;
    about_ = aboutInfo(kb_, tools_);
    ImGui::OpenPopup("###about");
  }
  if (!aboutTexture_ && about_) {
    if (SDL_Surface* icon = loadIconSurface()) {
      GLuint tex = 0;
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, icon->pitch / 4);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, icon->w, icon->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon->pixels);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      aboutTexture_ = tex;
      SDL_DestroySurface(icon);
    }
  }
  char title[64];
  snprintf(title, sizeof title, "%s###about", T("About VCSIDE"));
  float em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(em * 44, em * 38), ImGuiCond_Appearing);
  if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
  if (!about_) about_ = aboutInfo(kb_, tools_);
  const AboutInfo& a = *about_;
  if (aboutTexture_) {
    ImGui::Image((ImTextureID)(intptr_t)aboutTexture_, ImVec2(em * 6, em * 6));
    ImGui::SameLine(0, em);
  }
  ImGui::BeginGroup();
  ImGui::SetWindowFontScale(1.6f);
  ImGui::TextUnformatted("VCSIDE");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::TextUnformatted(F("Version {0}", {a.version}).c_str());
  ImGui::TextUnformatted(a.tagline.c_str());
  ImGui::TextDisabled("%s", a.credit.c_str());
  ImGui::TextDisabled("%s", a.copyright.c_str());
  ImGui::EndGroup();
  if (!kbError_.empty()) ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", kbError_.c_str());
  ImGui::Spacing();
  ImGui::BeginChild("credits", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f), ImGuiChildFlags_Borders);
  ImGui::PushTextWrapPos(0);
  for (auto& sec : a.sections) {
    ImGui::SeparatorText(sec.title.c_str());
    if (!sec.note.empty()) ImGui::TextDisabled("%s", sec.note.c_str());
    for (auto& l : sec.lines) {
      ImGui::PushID(&l);
      if (!l.url.empty()) ImGui::TextLinkOpenURL(l.name.c_str(), l.url.c_str());
      else ImGui::TextUnformatted(l.name.c_str());
      if (!l.detail.empty()) {
        ImGui::Indent(em);
        ImGui::TextDisabled("%s", l.detail.c_str());
        ImGui::Unindent(em);
      }
      ImGui::PopID();
    }
  }
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
  ImGui::TextDisabled("%s", F("Data: {0}", {a.dataDir}).c_str());
  if (ImGui::Button(T("Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
    about_.reset();
  }
  ImGui::EndPopup();
}

}  // namespace vcs
