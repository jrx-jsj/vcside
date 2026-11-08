// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "project.h"

#include <filesystem>
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

static std::vector<std::string> splitCommand(const std::string& s) {
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

std::vector<std::string> Project::buildArgs(Toolchain& tc, std::string* error) const {
  std::error_code ec;
  fs::create_directories(fs::path(outputPath()).parent_path(), ec);
  if (!buildCommand.empty()) return splitCommand(buildCommand);
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
      return {t->path, abs(main)};
    }
    case Language::ElfC:
      if (error) *error = T("ELF-VCS projects build with their Makefile: set \"build_command\" (e.g. \"make\") in the project.");
      return {};
    default:
      if (error) *error = T("Unknown project language.");
      return {};
  }
}

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
    if (!tv.empty() && tv != "AUTO") a.insert(a.end(), {"-format", tv});
  } else if (emulator == "gopher2600") {
    a.push_back(debug ? "DEBUG" : "RUN");
    if (!format.empty() && format != "AUTO") a.insert(a.end(), {"-mapping", format});
    if (!tv.empty() && tv != "AUTO") a.insert(a.end(), {"-tv", tv});
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
    bool pal = tvStd == "PAL" || tvStd == "SECAM";
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
    fs::path mainFile = fs::path(p.dir) / "main.bas";
    if (!fs::exists(mainFile))
      std::ofstream(mainFile) << "  rem " << projName << " - made with VCSIDE\n"
                              << "  COLUBK = $84\nmain\n  drawscreen\n  goto main\n";
  } else if (lang == Language::ElfC) {
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
