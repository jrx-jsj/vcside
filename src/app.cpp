// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "app.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "cycles.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "paths.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace vcs {

static const char* kTvIds[] = {"NTSC", "PAL", "PAL60", "SECAM"};
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

bool App::init(SDL_Window* window, ImFont* mono) {
  window_ = window;
  mono_ = mono;
  if (!kb_.load(dataDir() + "/kb", &kbError_)) SDL_Log("Knowledge base: %s", kbError_.c_str());
  loadSettings();
  tools_.detect();
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
  showCycles_ = j.value("show_cycles", true);
  tools_.loadSettings(j.value("tools", std::map<std::string, std::string>{}));
}

void App::saveSettings() {
  json j = {{"language", currentLanguage()},
            {"recent", recent_},
            {"show_cycles", showCycles_},
            {"tools", tools_.userPaths()}};
  std::ofstream(configDir() + "settings.json") << j.dump(2) << "\n";
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
  diagnostics_.clear();
  loadSymbols();
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
  f->asmSyntax = isAsmFile(path);
  f->editor.setText(readFile(path));
  f->editor.setGutterWidthChars(showCycles_ && f->asmSyntax ? 5 : 0);
  std::string ext = lower(fs::path(path).extension().string());
  bool cLike = ext == ".c" || ext == ".cpp" || ext == ".hpp" || (ext == ".h" && project_ && project_->language == Language::ElfC);
  if (cLike) f->asmSyntax = false;
  f->editor.setCommentPrefix(cLike ? "//" : ext == ".bas" ? "rem" : ";");
  if (f->asmSyntax) {
    EditorHooks h = asmHooks();
    OpenFile* self = f.get();
    h.gotoDefinition = [this, self](const std::string& w) { return gotoDefinition(w, self->path, self->editor.cursor().line); };
    f->editor.setHooks(h);
  }
  f->requestFocus = true;
  if (line > 0) f->editor.goTo(line - 1);
  lint(*f);
  files_.push_back(std::move(f));
  activeFile_ = (int)files_.size() - 1;
  return files_.back().get();
}

bool App::saveFile(OpenFile& f) {
  std::ofstream out(f.path, std::ios::binary);
  if (!out) {
    output_ += F("Cannot write {0}", {f.path}) + "\n";
    return false;
  }
  out << f.editor.text() << "\n";
  f.editor.markSaved();
  return true;
}

void App::saveAll() {
  for (auto& f : files_)
    if (f->editor.dirty()) saveFile(*f);
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
  std::string err;
  auto args = project_->buildArgs(tools_, &err);
  if (args.empty()) {
    output_ += err + "\n";
    lastStatus_ = err;
    outputScroll_ = true;
    return;
  }
  output_.clear();
  diagnostics_.clear();
  building_ = true;
  runAfterBuild_ = thenRun;
  lastStatus_ = T("Building...");
  job_.start(args, project_->dir);
}

void App::pollBuild() {
  if (!building_) return;
  output_ = job_.output();
  if (job_.running()) return;
  building_ = false;
  output_ = job_.output();
  bool gcc = project_ && (project_->language == Language::ElfC || !project_->buildCommand.empty());
  diagnostics_ = gcc ? parseGccOutput(output_) : parseDasmOutput(output_);
  if (gcc) {
    auto d = parseDasmOutput(output_);
    diagnostics_.insert(diagnostics_.end(), d.begin(), d.end());
  }
  bool ok = job_.exitCode() == 0;
  std::string buf = F(ok ? "Build succeeded (exit {0})" : "Build failed (exit {0})", {std::to_string(job_.exitCode())});
  output_ += "\n" + buf + "\n";
  lastStatus_ = buf;
  outputScroll_ = true;
  if (ok) {
    std::error_code ec;
    auto size = fs::file_size(project_->outputPath(), ec);
    if (!ec) output_ += F("{0}: {1} bytes", {project_->outputPath(), std::to_string(size)}) + "\n";
    loadSymbols();
  }
  for (auto& f : files_) lint(*f);
  if (ok && runAfterBuild_) run(runAfterBuild_ == 2);
  runAfterBuild_ = 0;
}

void App::run(bool debug) {
  if (!project_) return;
  std::string err;
  auto args = project_->runArgs(tools_, debug, &err);
  if (args.empty() || !launchDetached(args, project_->dir, &err)) {
    output_ += err + "\n";
    lastStatus_ = err;
  } else {
    output_ += "> " + quoteArgs(args) + "\n";
  }
  outputScroll_ = true;
}

void App::loadSymbols() {
  if (!project_) return;
  std::string sym = project_->stem() + ".sym";
  std::error_code ec;
  if (fs::exists(sym, ec)) symbols_ = parseDasmSymbols(readFile(sym));
}

std::optional<int> App::lookupSymbol(const std::string& name) const {
  auto it = symbols_.find(name);
  if (it != symbols_.end()) return it->second;
  if (const Register* r = kb_.reg(name)) {
    int a = r->addr();
    if (a >= 0) return r->access == "R" && a < 0x30 ? a + 0x30 : a;
  }
  return std::nullopt;
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
    if (const Register* r = kb_.reg(word)) {
      std::string s = r->name + " (" + r->chip + " " + r->address + ", " + (r->access == "R" ? T("read") : T("write")) + ")  " + r->bits + "\n" + r->summary;
      if (!r->timing.empty()) s += "\n" + F("Timing: {0}", {r->timing});
      for (auto& i : r->issues) s += "\n! " + i;
      return s;
    }
    auto it = symbols_.find(word);
    if (it != symbols_.end()) {
      char b[48];
      snprintf(b, sizeof b, "%s = $%04X (%d)", word.c_str(), it->second, it->second);
      return b;
    }
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
    std::string p = upper(prefix);
    for (auto& [name, v] : symbols_)
      if (upper(name).compare(0, p.size(), p) == 0 && out.size() < 50) out.push_back({name, T("symbol"), TokenKind::Symbol});
    return out;
  };
  return h;
}

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

bool App::gotoDefinition(const std::string& word, const std::string& fromFile, int fromLine) {
  if (word.empty()) return false;
  // 1. The open file: the closest definition above the cursor (local .labels repeat
  //    per SUBROUTINE), otherwise the first one below.
  for (auto& f : files_) {
    if (f->path != fromFile) continue;
    const auto& lines = f->editor.lines();
    int best = -1;
    for (int i = 0; i < (int)lines.size(); i++)
      if (definesWord(lines[i], word) && (best < 0 || i <= fromLine)) best = i;
    if (best >= 0) {
      openFile(f->path, best + 1);
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
    if (!it->is_regular_file() || !isAsmFile(it->path().string()) || it->path().string() == fromFile) continue;
    std::ifstream in(it->path());
    std::string line;
    for (int n = 1; std::getline(in, line); n++) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (definesWord(line, word)) {
        openFile(it->path().string(), n);
        return true;
      }
    }
  }
  lastStatus_ = F("No definition found for {0}", {word});
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
    if (f->editor.dirty()) {
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
      else if (kind.rfind("tool:", 0) == 0) {
        if (Tool* t = tools_.get(kind.substr(5))) t->userPath = path;
        tools_.detect();
        saveSettings();
      }
    }
  }
  pollBuild();

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
  docsPanel();
  newProjectDialog();
  toolsDialog();
  aboutDialog();
  closeDialogs();
}

void App::menuBar() {
  if (!ImGui::BeginMainMenuBar()) return;
  if (ImGui::BeginMenu(T("File"))) {
    if (ImGui::MenuItem(T("New project..."))) showNewProject_ = true;
    if (ImGui::MenuItem(T("Open project..."))) {
      static const SDL_DialogFileFilter filters[] = {{"VCSIDE project", "vcsproj"}, {"All files", "*"}};
      SDL_ShowOpenFileDialog(dialogCallback, new DialogCtx{this, "project"}, window_, filters, 2, nullptr, false);
    }
    if (ImGui::MenuItem(T("Open folder..."))) SDL_ShowOpenFolderDialog(dialogCallback, new DialogCtx{this, "folder"}, window_, nullptr, false);
    if (ImGui::BeginMenu(T("Recent projects"), !recent_.empty())) {
      for (auto r : recent_)
        if (ImGui::MenuItem(r.c_str())) openPath(r);
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(T("Save"), "Ctrl+S", false, activeFile_ >= 0)) saveFile(*files_[activeFile_]);
    if (ImGui::MenuItem(T("Save all"), "Ctrl+Shift+S")) saveAll();
    if (ImGui::MenuItem(T("Close file"), nullptr, false, activeFile_ >= 0)) closeRequest_ = activeFile_;
    ImGui::Separator();
    if (ImGui::MenuItem(T("Quit"))) requestQuit();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(T("Edit"), activeFile_ >= 0)) {
    auto& ed = files_[activeFile_]->editor;
    if (ImGui::MenuItem(T("Undo"), "Ctrl+Z")) ed.undo();
    if (ImGui::MenuItem(T("Redo"), "Ctrl+Shift+Z")) ed.redo();
    ImGui::Separator();
    if (ImGui::MenuItem(T("Cut"), "Ctrl+X")) ed.cut();
    if (ImGui::MenuItem(T("Copy"), "Ctrl+C")) ed.copy();
    if (ImGui::MenuItem(T("Paste"), "Ctrl+V")) ed.paste();
    if (ImGui::MenuItem(T("Select all"), "Ctrl+A")) ed.selectAll();
    ImGui::Separator();
    if (ImGui::MenuItem(T("Find..."), "Ctrl+F")) ed.openFind(false);
    if (ImGui::MenuItem(T("Replace..."), "Ctrl+H")) ed.openFind(true);
    if (ImGui::MenuItem(T("Find next"), "F3")) ed.findNext(false);
    if (ImGui::MenuItem(T("Find previous"), "Shift+F3")) ed.findNext(true);
    if (ImGui::MenuItem(T("Go to line..."), "Ctrl+G")) ed.openGoto();
    if (ImGui::MenuItem(T("Go to definition"), "F12", false, files_[activeFile_]->asmSyntax))
      gotoDefinition(ed.wordAtCursor(), files_[activeFile_]->path, ed.cursor().line);
    ImGui::Separator();
    if (ImGui::MenuItem(T("Toggle comment"), "Ctrl+/")) ed.toggleComment();
    if (ImGui::MenuItem(T("Duplicate lines"), "Ctrl+D")) ed.duplicateLines();
    if (ImGui::MenuItem(T("Move lines up"), "Alt+Up")) ed.moveLines(-1);
    if (ImGui::MenuItem(T("Move lines down"), "Alt+Down")) ed.moveLines(1);
    if (ImGui::MenuItem(T("Delete lines"), "Ctrl+Shift+K")) ed.deleteLines();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(T("Build"))) {
    bool p = project_.has_value() && !building_;
    if (ImGui::MenuItem(T("Build"), "F7", false, p)) build(0);
    if (ImGui::MenuItem(T("Build and run"), "F5", false, p)) build(1);
    if (ImGui::MenuItem(T("Build and debug"), "F6", false, p)) build(2);
    if (ImGui::MenuItem(T("Run"), nullptr, false, p)) run(false);
    if (project_) {
      ImGui::Separator();
      bool changed = false;
      if (ImGui::BeginMenu(T("Format"))) {
        for (auto& f : kb_.formats())
          if (ImGui::MenuItem(f.id.c_str(), f.name.c_str(), project_->format == f.id)) project_->format = f.id, changed = true;
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(T("TV standard"))) {
        for (auto tv : kTvIds)
          if (ImGui::MenuItem(tv, nullptr, project_->tv == tv)) project_->tv = tv, changed = true;
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(T("Emulator"))) {
        for (auto e : {"stella", "gopher2600"}) {
          Tool* t = tools_.get(e);
          if (ImGui::MenuItem(t->name.c_str(), t->found() ? T("found") : T("not found"), project_->emulator == e))
            project_->emulator = e, changed = true;
        }
        ImGui::EndMenu();
      }
      if (ImGui::BeginMenu(T("Illegal opcodes"))) {
        const IllegalOpcodes vals[] = {IllegalOpcodes::Allow, IllegalOpcodes::StableOnly, IllegalOpcodes::Warn, IllegalOpcodes::Forbid};
        const char* names[] = {N_("allow"), N_("stable only"), N_("warn"), N_("forbid")};
        for (int i = 0; i < 4; i++)
          if (ImGui::MenuItem(T(names[i]), nullptr, project_->illegal == vals[i])) project_->illegal = vals[i], changed = true;
        ImGui::EndMenu();
      }
      if (changed) {
        std::string err;
        project_->save(&err);
        for (auto& f : files_) lint(*f);
      }
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(T("View"))) {
    if (ImGui::MenuItem(T("Show cycle column"), nullptr, &showCycles_)) {
      for (auto& f : files_) f->editor.setGutterWidthChars(showCycles_ && f->asmSyntax ? 5 : 0);
      saveSettings();
    }
    if (ImGui::MenuItem(T("Reset layout"))) resetLayout_ = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(T("Settings"))) {
    if (ImGui::MenuItem(T("Tools"))) showTools_ = true;
    if (ImGui::BeginMenu(T("Language"))) {
      for (auto& l : languages()) {
        std::string hint = l.total > 0 && l.translated < l.total ? std::to_string(100 * l.translated / l.total) + " %" : "";
        if (ImGui::MenuItem(l.name.c_str(), hint.c_str(), currentLanguage() == l.code)) setLanguage(l.code), saveSettings();
      }
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(T("Help"))) {
    if (ImGui::MenuItem(T("Open documentation"), "F1")) focusDocs_ = true;
    if (ImGui::MenuItem(T("About VCSIDE"))) showAbout_ = true;
    ImGui::EndMenu();
  }
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
      ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1), "%s", buf.c_str());
      ImGui::SameLine();
      char sl[48];
      snprintf(sl, sizeof sl, "%.2f-%.2f", c.min / 76.0, c.max / 76.0);
      std::string extra = F("({0} scanlines)", {sl});
      if (c.unknown) extra += "  " + F("{0} unknown", {std::to_string(c.unknown)});
      if (c.guessed) extra += "  " + F("{0} guessed (~)", {std::to_string(c.guessed)});
      ImGui::TextDisabled("%s", extra.c_str());
    }
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
  std::function<void(const fs::path&)> tree = [&](const fs::path& dir) {
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
      std::string n = e.path().filename().string();
      if (e.is_directory()) {
        if (ImGui::TreeNode(n.c_str())) {
          tree(e.path());
          ImGui::TreePop();
        }
      } else {
        bool active = activeFile_ >= 0 && files_[activeFile_]->path == e.path().string();
        if (ImGui::Selectable(n.c_str(), active, ImGuiSelectableFlags_AllowDoubleClick)) {
          if (e.path().extension() == ".vcsproj") openProject(e.path().string());
          else if (e.path().extension() != ".bin") openFile(e.path().string());
        }
      }
    }
  };
  if (ImGui::TreeNodeEx(fs::path(folder_).filename().string().c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
    tree(folder_);
    ImGui::TreePop();
  }
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
      std::string label = fs::path(f.path).filename().string() + (f.editor.dirty() ? " *" : "") + "###" + f.path;
      ImGuiTabItemFlags fl = f.requestFocus ? ImGuiTabItemFlags_SetSelected : 0;
      if (f.editor.dirty()) fl |= ImGuiTabItemFlags_UnsavedDocument;
      if (ImGui::BeginTabItem(label.c_str(), &keep, fl)) {
        activeFile_ = i;
        if (f.requestFocus) {
          ImGui::SetKeyboardFocusHere();
          f.requestFocus = false;
        }
        if (f.editor.render("##code", mono_)) lint(f);
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
    if (!files_[closeRequest_]->editor.dirty()) {
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
  if (ImGui::Begin(title)) {
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
        ImVec4 col = d.severity == Diagnostic::Error ? ImVec4(1, 0.4f, 0.4f, 1) : d.severity == Diagnostic::Warning ? ImVec4(1, 0.8f, 0.3f, 1) : ImVec4(0.6f, 0.8f, 1, 1);
        ImGui::PushID((int)i);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        bool clicked = ImGui::Selectable(d.severity == Diagnostic::Error ? T("error") : d.severity == Diagnostic::Warning ? T("warning") : T("info"), false,
                                         ImGuiSelectableFlags_SpanAllColumns);
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::TableNextColumn();
        if (d.line) ImGui::Text("%s:%d", d.file.c_str(), d.line);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(d.message.c_str());
        if (clicked && d.line && project_) {
          fs::path p(d.file);
          if (!p.is_absolute()) p = fs::path(project_->dir) / p;
          openFile(p.string(), d.line);
        }
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

// Shows whether an entry is fully verified and where its facts come from.
static void unverifiedNotice(bool verified) {
  if (verified) return;
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.75f, 0.3f, 1));
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
  const ImVec4 legal(0.34f, 0.61f, 0.84f, 1), illegal(0.77f, 0.53f, 0.75f, 1), unstable(0.96f, 0.28f, 0.28f, 1), plain(0.85f, 0.85f, 0.85f, 1);
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
    for (auto& f : kb_.formats())
      if (match(f.id) || match(f.name)) item("F:" + f.id, f.id + "  " + f.name, plain);
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
      ImGui::TextColored(ImVec4(0.31f, 0.79f, 0.69f, 1), "%s", r->name.c_str());
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
  ImGui::Combo(T("TV standard"), &npTv_, kTvIds, 4);
  fs::path target = fs::path(npDir_) / npName_;
  ImGui::TextDisabled("%s", target.string().c_str());
  static std::string error;
  if (!error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error.c_str());
  if (ImGui::Button(T("Create"))) {
    Project p;
    error.clear();
    if (Project::create(target.string(), npName_, languageFromId(kLangIds[npLang_]), kTvIds[npTv_], &p, &error)) {
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
      if (t.found()) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "%s", t.path.c_str());
      else {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", T("not found"));
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

void App::aboutDialog() {
  if (showAbout_) {
    ImGui::OpenPopup("###about");
    showAbout_ = false;
  }
  char title[64];
  snprintf(title, sizeof title, "%s###about", T("About VCSIDE"));
  if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::Text("VCSIDE %s", VCSIDE_VERSION);
  ImGui::TextUnformatted(T("A small native IDE for Atari 2600 games."));
  ImGui::TextDisabled("Dear ImGui %s, SDL %d.%d.%d", IMGUI_VERSION, SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                      SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
  ImGui::TextDisabled("%s", F("Data: {0}", {dataDir()}).c_str());
  if (!kbError_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", kbError_.c_str());
  if (ImGui::Button(T("Close"))) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

}  // namespace vcs
