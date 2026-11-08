// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "project.h"

#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

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

bool Project::load(const std::string& path, std::string* error) {
  std::ifstream f(path);
  if (!f) {
    if (error) *error = F("Cannot open {0}", {path});
    return false;
  }
  try {
    json j = json::parse(f);
    file = fs::absolute(path).string();
    dir = fs::path(file).parent_path().string();
    name = j.value("name", fs::path(path).stem().string());
    language = languageFromId(j.value("language", "dasm"));
    main = j.value("main", main);
    output = j.value("output", output);
    format = j.value("format", format);
    tv = j.value("tv", tv);
    emulator = j.value("emulator", emulator);
    illegal = illegalFromId(j.value("illegal_opcodes", "warn"));
    includeDirs = j.value("include_dirs", std::vector<std::string>{});
    buildCommand = j.value("build_command", "");
    spaceWarnPercent = j.value("space_warn_percent", 90);
    stackReserve = j.value("stack_reserve", 8);
    emulatorOptions.clear();
    emulatorArgs.clear();
    if (j.contains("emulator_options") && j["emulator_options"].is_object())
      for (auto& [emu, opts] : j["emulator_options"].items())
        for (auto& [k, v] : opts.items())
          if (v.is_string() && !v.get<std::string>().empty()) emulatorOptions[emu][k] = v.get<std::string>();
    if (j.contains("emulator_args") && j["emulator_args"].is_object())
      for (auto& [emu, v] : j["emulator_args"].items())
        if (v.is_string() && !v.get<std::string>().empty()) emulatorArgs[emu] = v.get<std::string>();
  } catch (const std::exception& e) {
    if (error) *error = path + ": " + e.what();
    return false;
  }
  return true;
}

bool Project::save(std::string* error) const {
  json j = {{"name", name},           {"language", languageId(language)},
            {"main", main},           {"output", output},
            {"format", format},       {"tv", tv},
            {"emulator", emulator},   {"illegal_opcodes", illegalId(illegal)},
            {"include_dirs", includeDirs}};
  if (!buildCommand.empty()) j["build_command"] = buildCommand;
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
  std::ofstream f(file);
  if (!f) {
    if (error) *error = F("Cannot write {0}", {file});
    return false;
  }
  f << j.dump(2) << "\n";
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
  Tool* t = tc.get("dasm");
  if (!t || !t->found()) return {};
  std::vector<std::string> a = {t->path, abs(main), "-f2", "-v0", "-o" + stem() + ".ras", "-I" + dir};
  for (auto& d : includeDirs) a.push_back("-I" + abs(d));
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
    case Language::Dasm: return {"dasm"};
    case Language::BatariBasic: {
      // 1.9 is WebAssembly (2600basic.wasm next to its script) and runs on wasmtime
      Tool* bb = tc.get("bb");
      std::error_code ec;
      if (bb && bb->found() && fs::exists(fs::path(bb->path).parent_path() / "2600basic.wasm", ec)) return {"bb", "wasmtime"};
      return {"bb"};
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
    auto args = splitCommand(buildCommand);
    // a bare "make" is started from where VCSIDE found it (the PATH above is only the child's)
    if (Tool* m = tc.get("make"); !args.empty() && args[0] == "make" && m && m->found()) args[0] = m->path;
    return args;
  }
  switch (language) {
    case Language::Dasm: {
      Tool* t = tc.get("dasm");
      if (!t || !t->found()) {
        if (error) *error = T("DASM not found. Install it or set its path in Settings > Tools.");
        return {};
      }
      std::vector<std::string> a = {t->path, abs(main), "-f3", "-v0", "-o" + outputPath(),
                                    "-l" + stem() + ".lst", "-s" + stem() + ".sym", "-I" + dir};
      for (auto& d : includeDirs) a.push_back("-I" + abs(d));
      return a;
    }
    case Language::BatariBasic: {
      Tool* t = tc.get("bb");
      if (!t || !t->found()) {
        if (error) *error = T("batari Basic not found. Set the path to 2600basic.sh / 2600bas.bat in Settings > Tools.");
        return {};
      }
      // Its script runs in the source's folder (1.9 only lets wasmtime see "."), takes the
      // file name, and finds its own files through the bB variable. wasmtime has to be in
      // PATH: an app started from the desktop may not have the shell's PATH.
      fs::path src = abs(main);
      if (workDir) *workDir = src.parent_path().string();
      if (env) {
        env->push_back({"bB", fs::path(t->path).parent_path().string()});
        std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
        if (Tool* w = tc.get("wasmtime"); w && w->found()) path = fs::path(w->path).parent_path().string() + sep + path;
        env->push_back({"PATH", path});
      }
#ifdef _WIN32
      return {"cmd.exe", "/c", t->path, src.filename().string()};
#else
      return {"/bin/sh", t->path, src.filename().string()};
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

bool Project::create(const std::string& dirIn, const std::string& projName, Language lang, const std::string& tvStd,
                     Project* out, std::string* error) {
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
    std::string asmText = readFile((tpl / "main.asm").string());
    if (asmText.empty()) {
      if (error) *error = F("Template not found in {0}", {tpl.string()});
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
    fs::path hw = fs::path(p.dir) / "hardware.h";
    if (!fs::exists(hw)) fs::copy_file(tpl / "hardware.h", hw, ec);
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
    // data/templates/elf, copied whole; {{NAME}} in the Makefile and main.c
    fs::path tpl = fs::path(dataDir()) / "templates" / "elf";
    if (!fs::exists(tpl / "Makefile")) {
      if (error) *error = F("Template not found in {0}", {tpl.string()});
      return false;
    }
    for (auto& e : fs::recursive_directory_iterator(tpl, ec)) {
      if (!e.is_regular_file()) continue;
      fs::path rel = fs::relative(e.path(), tpl), dst = fs::path(p.dir) / rel;
      if (rel == "LICENSE.md" || fs::exists(dst)) continue;
      fs::create_directories(dst.parent_path(), ec);
      std::string text = readFile(e.path().string());
      replaceAll(text, "{{NAME}}", projName);
      std::ofstream(dst, std::ios::binary) << text;
    }
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
