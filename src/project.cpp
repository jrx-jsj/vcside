// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "project.h"

#include <filesystem>
#include <algorithm>
#include <regex>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "gfx.h"
#include "i18n.h"
#include "paths.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace vcs {

const char* languageId(Language l) {
  switch (l) {
    case Language::Dasm: return "dasm";
    case Language::BatariBasic: return "batari-basic";
    case Language::ElfC: return "elf-c";
    default: return "unknown";
  }
}
Language languageFromId(const std::string& s) {
  if (s == "dasm") return Language::Dasm;
  if (s == "batari-basic") return Language::BatariBasic;
  if (s == "elf-c") return Language::ElfC;
  return Language::Unknown;
}
const char* illegalId(IllegalOpcodes i) {
  switch (i) {
    case IllegalOpcodes::Allow: return "allow";
    case IllegalOpcodes::StableOnly: return "stable-only";
    case IllegalOpcodes::Forbid: return "forbid";
    default: return "warn";
  }
}
IllegalOpcodes illegalFromId(const std::string& s) {
  if (s == "allow") return IllegalOpcodes::Allow;
  if (s == "stable-only") return IllegalOpcodes::StableOnly;
  if (s == "forbid") return IllegalOpcodes::Forbid;
  return IllegalOpcodes::Warn;
}

// The keys VCSIDE writes; the others are kept as they are.
static const char* kKnownKeys[] = {"name", "language", "main", "output", "format", "tv", "emulator", "illegal_opcodes",
                                   "include_dirs", "build_command", "tool_paths", "build_flags", "space_warn_percent",
                                   "stack_reserve", "emulator_options", "emulator_args"};

bool Project::fromJson(const std::string& text, const std::string& path, std::string* error) {
  try {
    json j = json::parse(text);
    if (!j.is_object()) throw std::runtime_error(T("the project is not a JSON object"));
    Project p;
    p.file = fs::absolute(path).string();
    p.dir = fs::path(p.file).parent_path().string();
    p.name = j.value("name", fs::path(path).stem().string());
    p.language = languageFromId(j.value("language", "dasm"));
    p.main = j.value("main", p.main);
    p.output = j.value("output", p.output);
    p.format = j.value("format", p.format);
    p.tv = j.value("tv", p.tv);
    p.emulator = j.value("emulator", p.emulator);
    p.illegal = illegalFromId(j.value("illegal_opcodes", "warn"));
    p.includeDirs = j.value("include_dirs", std::vector<std::string>{});
    p.buildCommand = j.value("build_command", "");
    if (j.contains("tool_paths") && j["tool_paths"].is_object())
      for (auto& [k, v] : j["tool_paths"].items())
        if (v.is_string() && !v.get<std::string>().empty()) p.toolPaths[k] = v.get<std::string>();
    p.buildFlags = j.value("build_flags", "");
    p.spaceWarnPercent = j.value("space_warn_percent", 90);
    p.stackReserve = j.value("stack_reserve", 8);
    if (j.contains("emulator_options") && j["emulator_options"].is_object())
      for (auto& [emu, opts] : j["emulator_options"].items())
        for (auto& [k, v] : opts.items())
          if (v.is_string() && !v.get<std::string>().empty()) p.emulatorOptions[emu][k] = v.get<std::string>();
    if (j.contains("emulator_args") && j["emulator_args"].is_object())
      for (auto& [emu, v] : j["emulator_args"].items())
        if (v.is_string() && !v.get<std::string>().empty()) p.emulatorArgs[emu] = v.get<std::string>();
    for (auto& [k, v] : j.items())
      if (std::find(std::begin(kKnownKeys), std::end(kKnownKeys), k) == std::end(kKnownKeys)) p.otherKeys[k] = v.dump();
    *this = std::move(p);
  } catch (const std::exception& e) {
    if (error) *error = fs::path(path).filename().string() + ": " + e.what();
    return false;
  }
  return true;
}

bool Project::load(const std::string& path, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = F("Cannot open {0}", {path});
    return false;
  }
  std::ostringstream text;
  text << f.rdbuf();
  return fromJson(text.str(), path, error);
}

std::string Project::toJson() const {
  json j = {{"name", name},           {"language", languageId(language)},
            {"main", main},           {"output", output},
            {"format", format},       {"tv", tv},
            {"emulator", emulator},   {"illegal_opcodes", illegalId(illegal)},
            {"include_dirs", includeDirs}};
  if (!buildCommand.empty()) j["build_command"] = buildCommand;
  json tp = json::object();
  for (auto& [k, v] : toolPaths)
    if (!v.empty()) tp[k] = v;
  if (!tp.empty()) j["tool_paths"] = tp;
  if (!buildFlags.empty()) j["build_flags"] = buildFlags;
  if (spaceWarnPercent != 90) j["space_warn_percent"] = spaceWarnPercent;
  if (stackReserve != 8) j["stack_reserve"] = stackReserve;
  json eo = json::object(), ea = json::object();
  for (auto& [emu, opts] : emulatorOptions)
    for (auto& [k, v] : opts)
      if (!v.empty()) eo[emu][k] = v;
  for (auto& [emu, v] : emulatorArgs)
    if (!v.empty()) ea[emu] = v;
  if (!eo.empty()) j["emulator_options"] = eo;
  if (!ea.empty()) j["emulator_args"] = ea;
  for (auto& [k, v] : otherKeys) j[k] = json::parse(v, nullptr, false);
  return j.dump(2) + "\n";
}

bool Project::save(std::string* error) const {
  std::ofstream f(file, std::ios::binary);
  if (!f) {
    if (error) *error = F("Cannot write {0}", {file});
    return false;
  }
  f << toJson();
  return true;
}

std::string Project::abs(const std::string& rel) const {
  fs::path p(rel);
  return (p.is_absolute() ? p : fs::path(dir) / p).lexically_normal().string();
}

std::string Project::stem() const {
  fs::path p(outputPath());
  return (p.parent_path() / p.stem()).string();
}

std::vector<std::string> splitCommand(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  bool q = false;
  for (char c : s) {
    if (c == '"') q = !q;
    else if (!q && (c == ' ' || c == '\t')) {
      if (!cur.empty()) out.push_back(cur), cur.clear();
    } else cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<std::string> Project::rasArgs(Toolchain& tc) const {
  if (language != Language::Dasm || !buildCommand.empty()) return {};
  std::string dasm = toolPath(tc, "dasm");
  if (dasm.empty()) return {};
  std::vector<std::string> a = {dasm, abs(main), "-f2", "-v0", "-o" + stem() + ".ras", "-I" + dir};
  for (auto& d : includeDirs) a.push_back("-I" + abs(d));
  for (auto& f : splitCommand(buildFlags))
    if (f.rfind("-D", 0) == 0 || f.rfind("-M", 0) == 0 || f.rfind("-I", 0) == 0) a.push_back(f);  // what changes the code
  return a;
}

std::vector<std::string> Project::buildTools(Toolchain& tc) const {
  if (!buildCommand.empty()) {
    if (language != Language::ElfC) return {};
    std::vector<std::string> out = {"arm-gcc"};
    auto words = splitCommand(buildCommand);
    if (!words.empty() && fs::path(words[0]).stem().string().find("make") != std::string::npos) out.push_back("make");
    return out;
  }
  switch (language) {
    case Language::Dasm: return toolPaths.count("dasm") && !toolPath(tc, "dasm").empty() ? std::vector<std::string>{} : std::vector<std::string>{"dasm"};
    case Language::BatariBasic: {
      // 1.9 is WebAssembly (2600basic.wasm next to its script) and runs on wasmtime
      std::string bb = toolPath(tc, "bb");
      bool own = toolPaths.count("bb") && !bb.empty();
      std::error_code ec;
      std::vector<std::string> out;
      if (!own) out.push_back("bb");
      if (!bb.empty() && fs::exists(fs::path(bb).parent_path() / "2600basic.wasm", ec)) out.push_back("wasmtime");
      return out;
    }
    default: return {};
  }
}

std::vector<std::string> Project::buildArgs(Toolchain& tc, std::string* error, std::string* workDir,
                                            EnvVars* env) const {
  std::error_code ec;
  if (workDir) *workDir = dir;
  fs::create_directories(fs::path(outputPath()).parent_path(), ec);
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif
  if (!buildCommand.empty()) {
    // make and what it calls (arm-none-eabi-gcc, dasm) are found through PATH, and an app
    // opened from the desktop may not have the shell's: the folders where VCSIDE found
    // them go first.
    if (env) {
      std::string path = std::getenv("PATH") ? std::getenv("PATH") : "", extra;
      for (auto id : {"make", "arm-gcc", "dasm"}) {
        Tool* t = tc.get(id);
        if (!t || !t->found()) continue;
        std::string d = fs::path(t->path).parent_path().string();
        if ((sep + extra + sep).find(sep + d + sep) == std::string::npos) extra += (extra.empty() ? "" : std::string(1, sep)) + d;
      }
      if (!extra.empty()) env->push_back({"PATH", extra + sep + path});
    }
    // several commands, pipes or redirections: the system's shell runs the line
    if (buildCommand.find_first_of("&|;><") != std::string::npos) {
#ifdef _WIN32
      return {"cmd.exe", "/c", buildCommand};
#else
      return {"/bin/sh", "-c", buildCommand};
#endif
    }
    auto args = splitCommand(buildCommand);
    // a bare "make" is started from where VCSIDE found it (the PATH above is only the child's)
    if (Tool* m = tc.get("make"); !args.empty() && args[0] == "make" && m && m->found()) args[0] = m->path;
    return args;
  }
  switch (language) {
    case Language::Dasm: {
      std::string dasm = toolPath(tc, "dasm");
      if (dasm.empty()) {
        if (error) *error = toolPaths.count("dasm") ? F("The project's DASM ({0}) does not exist.", {toolPaths.at("dasm")})
                                                    : std::string(T("DASM not found. Install it or set its path in Settings > Tools."));
        return {};
      }
      std::vector<std::string> a = {dasm, abs(main), "-f3", "-v0", "-o" + outputPath(),
                                    "-l" + stem() + ".lst", "-s" + stem() + ".sym", "-I" + dir};
      for (auto& d : includeDirs) a.push_back("-I" + abs(d));
      for (auto& f : splitCommand(buildFlags)) a.push_back(f);
      return a;
    }
    case Language::BatariBasic: {
      std::string bbPath = toolPath(tc, "bb");
      if (bbPath.empty()) {
        if (error) *error = toolPaths.count("bb") ? F("The project's batari Basic ({0}) does not exist.", {toolPaths.at("bb")})
                                                  : std::string(T("batari Basic not found. Set the path to 2600basic.sh / 2600bas.bat in Settings > Tools."));
        return {};
      }
      // Its script runs in the source's folder (1.9 only lets wasmtime see "."), takes the
      // file name, and finds its own files through the bB variable. wasmtime has to be in
      // PATH: an app started from the desktop may not have the shell's PATH.
      fs::path src = abs(main);
      if (workDir) *workDir = src.parent_path().string();
      if (env) {
        env->push_back({"bB", fs::path(bbPath).parent_path().string()});
        std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
        if (Tool* w = tc.get("wasmtime"); w && w->found()) path = fs::path(w->path).parent_path().string() + sep + path;
        env->push_back({"PATH", path});
      }
#ifdef _WIN32
      return {"cmd.exe", "/c", bbPath, src.filename().string()};
#else
      return {"/bin/sh", bbPath, src.filename().string()};
#endif
    }
    case Language::ElfC:
      if (error) *error = T("ELF-VCS projects build with their Makefile: set \"build_command\" (e.g. \"make\") in the project.");
      return {};
    default:
      if (error) *error = T("Unknown project language.");
      return {};
  }
}

std::string Project::toolPath(Toolchain& tc, const std::string& id) const {
  std::error_code ec;
  if (auto it = toolPaths.find(id); it != toolPaths.end() && !it->second.empty()) {
    std::string p = abs(it->second);
    return fs::is_regular_file(p, ec) ? p : std::string();
  }
  Tool* t = tc.get(id);
  return t && t->found() ? t->path : std::string();
}

static std::string readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}

bool Project::detect(const std::string& dirIn, Project* out) {
  std::error_code ec;
  fs::path d = fs::absolute(dirIn);
  std::string command, script;
  for (auto n : {"Makefile", "makefile", "GNUmakefile"})
    if (fs::is_regular_file(d / n, ec)) {
      command = "make", script = n;
      break;
    }
#ifdef _WIN32
  const char* scripts[] = {"build.bat", "make.bat", "build.cmd", "build.sh"};
#else
  const char* scripts[] = {"build.sh", "make.sh", "build.bat", "make.bat"};
#endif
  if (command.empty())
    for (auto n : scripts)
      if (fs::is_regular_file(d / n, ec)) {
        std::string ext = fs::path(n).extension().string();
        command = ext == ".sh" ? std::string("sh ") + n : std::string(n);
        script = n;
        break;
      }
  if (command.empty()) return false;
  Project p;
  p.dir = d.string();
  p.name = d.filename().string();
  p.file = (d / (p.name + ".vcsproj")).string();
  p.buildCommand = command;
  // the main source: main.* first, else a file with the 6502's processor line, else any
  std::string asmMain, basMain, cMain;
  std::vector<fs::path> files;
  for (auto& e : fs::directory_iterator(d, ec))
    if (e.is_regular_file(ec)) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (auto& f : files) {
    std::string ext = f.extension().string(), stem = f.stem().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    bool isMain = stem == "main";
    if (ext == ".asm" || ext == ".s" || ext == ".a") {
      if (isMain || (asmMain.empty() && readAll(f).find("processor") != std::string::npos)) asmMain = f.filename().string();
    } else if (ext == ".bas" && (isMain || basMain.empty())) {
      basMain = f.filename().string();
    } else if (ext == ".c" && (isMain || cMain.empty())) {
      cMain = f.filename().string();
    }
  }
  if (!cMain.empty() && asmMain.empty() && basMain.empty()) p.language = Language::ElfC, p.main = cMain;
  else if (!basMain.empty() && asmMain.empty()) p.language = Language::BatariBasic, p.main = basMain;
  else p.language = Language::Dasm, p.main = asmMain.empty() ? (basMain.empty() ? cMain : basMain) : asmMain;
  // the ROM: the first word of the build file that names a .bin, .a26 or .elf
  std::string text = readAll(d / script);
  static const std::regex rom(R"(([A-Za-z0-9_./\-]+)\.(bin|a26|elf)\b)");
  for (std::sregex_iterator it(text.begin(), text.end(), rom), end; it != end; ++it) {
    std::string m = (*it)[0];
    size_t at = (size_t)it->position(0);
    if (at > 0 && (text[at - 1] == ')' || text[at - 1] == '}' || text[at - 1] == '%')) continue;  // $(NAME).bin, %.bin
    if (m.rfind("-o", 0) == 0) m.erase(0, 2);  // dasm -oout/game.bin
    if (m.empty() || m[0] == '-') continue;
    p.output = m;
    break;
  }
  if (p.output == Project().output) p.output = fs::path(p.main).stem().string() + ".bin";
  *out = p;
  return true;
}

const std::vector<std::string>& tvStandardIds() {
  static const std::vector<std::string> ids = {"NTSC", "PAL", "PAL60", "SECAM", "NTSC50", "SECAM60", "PAL-M", "AUTO"};
  return ids;
}

// Stella's -format and Gopher2600's -tv (checked in their -help and sources).
bool tvSupported(const std::string& emulator, const std::string& tv) {
  if (tv == "NTSC" || tv == "PAL" || tv == "PAL60" || tv == "SECAM" || tv == "AUTO") return true;
  if (emulator == "stella") return tv == "NTSC50" || tv == "SECAM60";
  if (emulator == "gopher2600") return tv == "PAL-M";
  return false;
}

// 312-line frames (the rest have 262): PAL, SECAM and NTSC colours at 50 Hz.
bool tvIs50Hz(const std::string& tv) { return tv == "PAL" || tv == "SECAM" || tv == "NTSC50"; }

std::vector<std::string> Project::runArgs(Toolchain& tc, bool debug, std::string* error) const {
  std::string rom = outputPath();
  if (!fs::exists(rom)) {
    if (error) *error = F("The ROM does not exist yet: build first ({0}).", {rom});
    return {};
  }
  Tool* t = tc.get(emulator);
  if (!t || !t->found()) {
    if (error) *error = F("{0} not found. Install it or set its path in Settings > Tools.", {t ? t->name : emulator});
    return {};
  }
  std::vector<std::string> a = {t->path};
  if (emulator == "stella") {
    if (debug) a.push_back("-debug");
    if (!format.empty() && format != "AUTO") a.insert(a.end(), {"-bs", stellaFormat(format)});
    if (!tv.empty() && tv != "AUTO" && tvSupported(emulator, tv)) a.insert(a.end(), {"-format", tv});
  } else if (emulator == "gopher2600") {
    a.push_back(debug ? "DEBUG" : "RUN");
    if (!format.empty() && format != "AUTO") a.insert(a.end(), {"-mapping", format});
    if (!tv.empty() && tv != "AUTO" && tvSupported(emulator, tv)) a.insert(a.end(), {"-tv", tv});
  }
  a.push_back(rom);
  return a;
}

static std::string readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static void replaceAll(std::string& s, const std::string& a, const std::string& b) {
  for (size_t pos = 0; (pos = s.find(a, pos)) != std::string::npos; pos += b.size()) s.replace(pos, a.size(), b);
}

std::vector<std::pair<std::string, const char*>> Project::examples(Language lang) {
  if (lang == Language::Dasm || lang == Language::ElfC) return {{"animation", N_("Example: an animated sprite")}};
  return {};
}

// A .vcsgfx of an example comes with the tables its program includes (.gfx.asm, or .gfx.h
// for C), written as the graphics editor writes them on save.
static void writeGfxIncludes(const fs::path& dir, bool c) {
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".vcsgfx") continue;
    gfx::Asset a;
    std::string err;
    if (!gfx::fromJson(readFile(e.path().string()), &a, &err)) continue;
    std::string src = e.path().filename().string();
    std::ofstream(dir / (e.path().stem().string() + (c ? ".gfx.h" : ".gfx.asm")), std::ios::binary)
        << (c ? gfx::cInclude(a, src) : gfx::dasmInclude(a, src));
  }
}

bool Project::create(const std::string& dirIn, const std::string& projName, Language lang, const std::string& tvStd,
                     Project* out, std::string* error, const std::string& example) {
  fs::path d(dirIn);
  std::error_code ec;
  fs::create_directories(d, ec);
  if (ec) {
    if (error) *error = F("Cannot create {0}: {1}", {d.string(), ec.message()});
    return false;
  }
  Project p;
  p.dir = fs::absolute(d).string();
  p.file = (fs::path(p.dir) / (projName + ".vcsproj")).string();
  p.name = projName;
  p.language = lang;
  p.tv = tvStd;
  p.output = "build/" + projName + ".bin";
  if (fs::exists(p.file)) {
    if (error) *error = F("{0} already exists.", {p.file});
    return false;
  }
  if (lang == Language::Dasm) {
    fs::path tpl = fs::path(dataDir()) / "templates" / "dasm";
    fs::path ex = example.empty() ? tpl : fs::path(dataDir()) / "templates" / ("dasm-" + example);
    std::string asmText = readFile((ex / "main.asm").string());
    if (asmText.empty()) {
      if (error) *error = F("Template not found in {0}", {ex.string()});
      return false;
    }
    bool pal = tvIs50Hz(tvStd);
    std::string defs = pal ? "VBLANK_LINES   = 45\nVISIBLE_LINES  = 228\nOVERSCAN_LINES = 36"
                           : "VBLANK_LINES   = 37\nVISIBLE_LINES  = 192\nOVERSCAN_LINES = 30";
    replaceAll(asmText, "{{NAME}}", projName);
    replaceAll(asmText, "{{TV}}", tvStd);
    replaceAll(asmText, "{{TVDEFS}}", defs);
    fs::path mainFile = fs::path(p.dir) / "main.asm";
    if (!fs::exists(mainFile)) std::ofstream(mainFile) << asmText;
    fs::path hw = fs::path(p.dir) / "2600.h";
    if (!fs::exists(hw)) fs::copy_file(tpl / "2600.h", hw, ec);
    // the example's graphics, and the tables they give
    for (auto& e : fs::directory_iterator(ex, ec))
      if (e.path().extension() == ".vcsgfx" && !fs::exists(fs::path(p.dir) / e.path().filename()))
        fs::copy_file(e.path(), fs::path(p.dir) / e.path().filename(), ec);
    writeGfxIncludes(p.dir, false);
    p.main = "main.asm";
  } else if (lang == Language::BatariBasic) {
    p.main = "main.bas";
    p.output = "main.bas.bin";
    p.format = "AUTO";  // the game says it (set romsize, set kernel); Stella and Gopher2600 detect it
    fs::path tpl = fs::path(dataDir()) / "templates" / "bb" / "main.bas";
    std::string text = readFile(tpl.string());
    if (text.empty()) {
      if (error) *error = F("Template not found in {0}", {tpl.string()});
      return false;
    }
    replaceAll(text, "{{NAME}}", projName);
    fs::path mainFile = fs::path(p.dir) / "main.bas";
    if (!fs::exists(mainFile)) std::ofstream(mainFile, std::ios::binary) << text;
  } else if (lang == Language::ElfC) {
    if (projName.find_first_of(" \t") != std::string::npos) {
      if (error) *error = T("ELF projects build with make, which does not take spaces in file names: choose a name without spaces.");
      return false;
    }
    // data/templates/elf, copied whole, with an example's files over it; {{NAME}} in the
    // Makefile and main.c
    fs::path tpl = fs::path(dataDir()) / "templates" / "elf";
    fs::path ex = fs::path(dataDir()) / "templates" / ("elf-" + example);
    if (!fs::exists(tpl / "Makefile") || (!example.empty() && !fs::exists(ex / "src" / "main.c"))) {
      if (error) *error = F("Template not found in {0}", {(example.empty() ? tpl : ex).string()});
      return false;
    }
    std::vector<fs::path> from{tpl};
    if (!example.empty()) from.insert(from.begin(), ex);  // the first copy of a file wins
    for (auto& t : from)
      for (auto& e : fs::recursive_directory_iterator(t, ec)) {
        if (!e.is_regular_file()) continue;
        fs::path rel = fs::relative(e.path(), t), dst = fs::path(p.dir) / rel;
        if (rel == "LICENSE.md" || fs::exists(dst)) continue;
        fs::create_directories(dst.parent_path(), ec);
        std::string text = readFile(e.path().string());
        replaceAll(text, "{{NAME}}", projName);
        std::ofstream(dst, std::ios::binary) << text;
      }
    writeGfxIncludes(fs::path(p.dir) / "src", true);
    p.main = "src/main.c";
    p.buildCommand = "make";
    p.format = "ELF";
  }
  if (!p.save(error)) return false;
  if (out) *out = p;
  return true;
}

std::string stellaFormat(const std::string& id) {
  // VCSIDE ids follow Stella's names, except a few aliases.
  if (id == "DPC+") return "DPCP";
  if (id == "CDFJ+") return "CDFJ+";
  if (id == "3E+") return "3EP";
  return id;
}

}  // namespace vcs
