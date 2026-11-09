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

const char* languageId(Language language) {
  switch (language) {
    case Language::Dasm: return "dasm";
    case Language::BatariBasic: return "batari-basic";
    case Language::ElfC: return "elf-c";
    default: return "unknown";
  }
}
Language languageFromId(const std::string& id) {
  if (id == "dasm") return Language::Dasm;
  if (id == "batari-basic") return Language::BatariBasic;
  if (id == "elf-c") return Language::ElfC;
  return Language::Unknown;
}
const char* illegalId(IllegalOpcodes policy) {
  switch (policy) {
    case IllegalOpcodes::Allow: return "allow";
    case IllegalOpcodes::StableOnly: return "stable-only";
    case IllegalOpcodes::Forbid: return "forbid";
    default: return "warn";
  }
}
IllegalOpcodes illegalFromId(const std::string& id) {
  if (id == "allow") return IllegalOpcodes::Allow;
  if (id == "stable-only") return IllegalOpcodes::StableOnly;
  if (id == "forbid") return IllegalOpcodes::Forbid;
  return IllegalOpcodes::Warn;
}

// The keys VCSIDE writes; the others are kept as they are.
static const char* kKnownKeys[] = {"name", "language", "main", "output", "format", "tv", "emulator", "illegal_opcodes",
                                   "include_dirs", "build_command", "tool_paths", "build_flags", "space_warn_percent",
                                   "stack_reserve", "emulator_options", "emulator_args"};

bool Project::fromJson(const std::string& text, const std::string& path, std::string* error) {
  try {
    json document = json::parse(text);
    if (!document.is_object()) throw std::runtime_error(T("the project is not a JSON object"));
    Project parsed;
    parsed.file = fs::absolute(path).string();
    parsed.dir = fs::path(parsed.file).parent_path().string();
    parsed.name = document.value("name", fs::path(path).stem().string());
    parsed.language = languageFromId(document.value("language", "dasm"));
    parsed.main = document.value("main", parsed.main);
    parsed.output = document.value("output", parsed.output);
    parsed.format = document.value("format", parsed.format);
    parsed.tv = document.value("tv", parsed.tv);
    parsed.emulator = document.value("emulator", parsed.emulator);
    parsed.illegal = illegalFromId(document.value("illegal_opcodes", "warn"));
    parsed.includeDirs = document.value("include_dirs", std::vector<std::string>{});
    parsed.buildCommand = document.value("build_command", "");
    if (document.contains("tool_paths") && document["tool_paths"].is_object())
      for (auto& [key, value] : document["tool_paths"].items())
        if (value.is_string() && !value.get<std::string>().empty()) parsed.toolPaths[key] = value.get<std::string>();
    parsed.buildFlags = document.value("build_flags", "");
    parsed.spaceWarnPercent = document.value("space_warn_percent", 90);
    parsed.stackReserve = document.value("stack_reserve", 8);
    if (document.contains("emulator_options") && document["emulator_options"].is_object())
      for (auto& [emulatorId, options] : document["emulator_options"].items())
        for (auto& [key, value] : options.items())
          if (value.is_string() && !value.get<std::string>().empty()) parsed.emulatorOptions[emulatorId][key] = value.get<std::string>();
    if (document.contains("emulator_args") && document["emulator_args"].is_object())
      for (auto& [emulatorId, value] : document["emulator_args"].items())
        if (value.is_string() && !value.get<std::string>().empty()) parsed.emulatorArgs[emulatorId] = value.get<std::string>();
    for (auto& [key, value] : document.items())
      if (std::find(std::begin(kKnownKeys), std::end(kKnownKeys), key) == std::end(kKnownKeys)) parsed.otherKeys[key] = value.dump();
    *this = std::move(parsed);
  } catch (const std::exception& exception) {
    if (error) *error = fs::path(path).filename().string() + ": " + exception.what();
    return false;
  }
  return true;
}

bool Project::load(const std::string& path, std::string* error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    if (error) *error = F("Cannot open {0}", {path});
    return false;
  }
  std::ostringstream text;
  text << stream.rdbuf();
  return fromJson(text.str(), path, error);
}

std::string Project::toJson() const {
  json document = {{"name", name},           {"language", languageId(language)},
            {"main", main},           {"output", output},
            {"format", format},       {"tv", tv},
            {"emulator", emulator},   {"illegal_opcodes", illegalId(illegal)},
            {"include_dirs", includeDirs}};
  if (!buildCommand.empty()) document["build_command"] = buildCommand;
  json toolPathsJson = json::object();
  for (auto& [key, value] : toolPaths)
    if (!value.empty()) toolPathsJson[key] = value;
  if (!toolPathsJson.empty()) document["tool_paths"] = toolPathsJson;
  if (!buildFlags.empty()) document["build_flags"] = buildFlags;
  if (spaceWarnPercent != 90) document["space_warn_percent"] = spaceWarnPercent;
  if (stackReserve != 8) document["stack_reserve"] = stackReserve;
  json optionsJson = json::object(), argsJson = json::object();
  for (auto& [emulatorId, options] : emulatorOptions)
    for (auto& [key, value] : options)
      if (!value.empty()) optionsJson[emulatorId][key] = value;
  for (auto& [emulatorId, value] : emulatorArgs)
    if (!value.empty()) argsJson[emulatorId] = value;
  if (!optionsJson.empty()) document["emulator_options"] = optionsJson;
  if (!argsJson.empty()) document["emulator_args"] = argsJson;
  for (auto& [key, value] : otherKeys) document[key] = json::parse(value, nullptr, false);
  return document.dump(2) + "\n";
}

bool Project::save(std::string* error) const {
  std::ofstream stream(file, std::ios::binary);
  if (!stream) {
    if (error) *error = F("Cannot write {0}", {file});
    return false;
  }
  stream << toJson();
  return true;
}

std::string Project::abs(const std::string& relativePath) const {
  fs::path path(relativePath);
  return (path.is_absolute() ? path : fs::path(dir) / path).lexically_normal().string();
}

std::string Project::stem() const {
  fs::path outputFile(outputPath());
  return (outputFile.parent_path() / outputFile.stem()).string();
}

std::vector<std::string> splitCommand(const std::string& command) {
  std::vector<std::string> words;
  std::string current;
  bool inQuotes = false;
  for (char character : command) {
    if (character == '"') inQuotes = !inQuotes;
    else if (!inQuotes && (character == ' ' || character == '\t')) {
      if (!current.empty()) words.push_back(current), current.clear();
    } else current += character;
  }
  if (!current.empty()) words.push_back(current);
  return words;
}

std::vector<std::string> Project::rasArgs(Toolchain& toolchain) const {
  if (language != Language::Dasm || !buildCommand.empty()) return {};
  std::string dasm = toolPath(toolchain, "dasm");
  if (dasm.empty()) return {};
  std::vector<std::string> arguments = {dasm, abs(main), "-f2", "-v0", "-o" + stem() + ".ras", "-I" + dir};
  for (auto& includeDir : includeDirs) arguments.push_back("-I" + abs(includeDir));
  for (auto& flag : splitCommand(buildFlags))
    if (flag.rfind("-D", 0) == 0 || flag.rfind("-M", 0) == 0 || flag.rfind("-I", 0) == 0) arguments.push_back(flag);  // what changes the code
  return arguments;
}

std::vector<std::string> Project::buildTools(Toolchain& toolchain) const {
  if (!buildCommand.empty()) {
    if (language != Language::ElfC) return {};
    std::vector<std::string> tools = {"arm-gcc"};
    auto words = splitCommand(buildCommand);
    if (!words.empty() && fs::path(words[0]).stem().string().find("make") != std::string::npos) tools.push_back("make");
    return tools;
  }
  switch (language) {
    case Language::Dasm: return toolPaths.count("dasm") && !toolPath(toolchain, "dasm").empty() ? std::vector<std::string>{} : std::vector<std::string>{"dasm"};
    case Language::BatariBasic: {
      // 1.9 is WebAssembly (2600basic.wasm next to its script) and runs on wasmtime
      std::string bb = toolPath(toolchain, "bb");
      bool hasOwnBb = toolPaths.count("bb") && !bb.empty();
      std::error_code errorCode;
      std::vector<std::string> tools;
      if (!hasOwnBb) tools.push_back("bb");
      if (!bb.empty() && fs::exists(fs::path(bb).parent_path() / "2600basic.wasm", errorCode)) tools.push_back("wasmtime");
      return tools;
    }
    default: return {};
  }
}

std::vector<std::string> Project::buildArgs(Toolchain& toolchain, std::string* error, std::string* workDir,
                                            EnvVars* env) const {
  std::error_code errorCode;
  if (workDir) *workDir = dir;
  fs::create_directories(fs::path(outputPath()).parent_path(), errorCode);
#ifdef _WIN32
  const char pathSeparator = ';';
#else
  const char pathSeparator = ':';
#endif
  if (!buildCommand.empty()) {
    // make and what it calls (arm-none-eabi-gcc, dasm) are found through PATH, and an app
    // opened from the desktop may not have the shell's: the folders where VCSIDE found
    // them go first.
    if (env) {
      std::string path = std::getenv("PATH") ? std::getenv("PATH") : "", extra;
      for (auto id : {"make", "arm-gcc", "dasm"}) {
        Tool* tool = toolchain.get(id);
        if (!tool || !tool->found()) continue;
        std::string folder = fs::path(tool->path).parent_path().string();
        if ((pathSeparator + extra + pathSeparator).find(pathSeparator + folder + pathSeparator) == std::string::npos) extra += (extra.empty() ? "" : std::string(1, pathSeparator)) + folder;
      }
      if (!extra.empty()) env->push_back({"PATH", extra + pathSeparator + path});
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
    if (Tool* make = toolchain.get("make"); !args.empty() && args[0] == "make" && make && make->found()) args[0] = make->path;
    return args;
  }
  switch (language) {
    case Language::Dasm: {
      std::string dasm = toolPath(toolchain, "dasm");
      if (dasm.empty()) {
        if (error) *error = toolPaths.count("dasm") ? F("The project's DASM ({0}) does not exist.", {toolPaths.at("dasm")})
                                                    : std::string(T("DASM not found. Install it or set its path in Settings > Tools."));
        return {};
      }
      std::vector<std::string> arguments = {dasm, abs(main), "-f3", "-v0", "-o" + outputPath(),
                                    "-l" + stem() + ".lst", "-s" + stem() + ".sym", "-I" + dir};
      for (auto& includeDir : includeDirs) arguments.push_back("-I" + abs(includeDir));
      for (auto& flag : splitCommand(buildFlags)) arguments.push_back(flag);
      return arguments;
    }
    case Language::BatariBasic: {
      std::string bbPath = toolPath(toolchain, "bb");
      if (bbPath.empty()) {
        if (error) *error = toolPaths.count("bb") ? F("The project's batari Basic ({0}) does not exist.", {toolPaths.at("bb")})
                                                  : std::string(T("batari Basic not found. Set the path to 2600basic.sh / 2600bas.bat in Settings > Tools."));
        return {};
      }
      // Its script runs in the source's folder (1.9 only lets wasmtime see "."), takes the
      // file name, and finds its own files through the bB variable. wasmtime has to be in
      // PATH: an app started from the desktop may not have the shell's PATH.
      fs::path source = abs(main);
      if (workDir) *workDir = source.parent_path().string();
      if (env) {
        env->push_back({"bB", fs::path(bbPath).parent_path().string()});
        std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
        if (Tool* wasmtime = toolchain.get("wasmtime"); wasmtime && wasmtime->found()) path = fs::path(wasmtime->path).parent_path().string() + pathSeparator + path;
        env->push_back({"PATH", path});
      }
#ifdef _WIN32
      return {"cmd.exe", "/c", bbPath, source.filename().string()};
#else
      return {"/bin/sh", bbPath, source.filename().string()};
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

std::string Project::toolPath(Toolchain& toolchain, const std::string& id) const {
  std::error_code errorCode;
  if (auto found = toolPaths.find(id); found != toolPaths.end() && !found->second.empty()) {
    std::string resolved = abs(found->second);
    return fs::is_regular_file(resolved, errorCode) ? resolved : std::string();
  }
  Tool* tool = toolchain.get(id);
  return tool && tool->found() ? tool->path : std::string();
}

static std::string readAll(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

bool Project::detect(const std::string& dirIn, Project* result) {
  std::error_code errorCode;
  fs::path directory = fs::absolute(dirIn);
  std::string command, script;
  for (auto candidate : {"Makefile", "makefile", "GNUmakefile"})
    if (fs::is_regular_file(directory / candidate, errorCode)) {
      command = "make", script = candidate;
      break;
    }
#ifdef _WIN32
  const char* scripts[] = {"build.bat", "make.bat", "build.cmd", "build.sh"};
#else
  const char* scripts[] = {"build.sh", "make.sh", "build.bat", "make.bat"};
#endif
  if (command.empty())
    for (auto candidate : scripts)
      if (fs::is_regular_file(directory / candidate, errorCode)) {
        std::string ext = fs::path(candidate).extension().string();
        command = ext == ".sh" ? std::string("sh ") + candidate : std::string(candidate);
        script = candidate;
        break;
      }
  if (command.empty()) return false;
  Project found;
  found.dir = directory.string();
  found.name = directory.filename().string();
  found.file = (directory / (found.name + ".vcsproj")).string();
  found.buildCommand = command;
  // the main source: main.* first, else a file with the 6502's processor line, else any
  std::string asmMain, basMain, cMain;
  std::vector<fs::path> files;
  for (auto& entry : fs::directory_iterator(directory, errorCode))
    if (entry.is_regular_file(errorCode)) files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  for (auto& file : files) {
    std::string ext = file.extension().string(), stem = file.stem().string();
    for (auto& character : ext) character = (char)std::tolower((unsigned char)character);
    bool isMain = stem == "main";
    if (ext == ".asm" || ext == ".s" || ext == ".a") {
      if (isMain || (asmMain.empty() && readAll(file).find("processor") != std::string::npos)) asmMain = file.filename().string();
    } else if (ext == ".bas" && (isMain || basMain.empty())) {
      basMain = file.filename().string();
    } else if (ext == ".c" && (isMain || cMain.empty())) {
      cMain = file.filename().string();
    }
  }
  if (!cMain.empty() && asmMain.empty() && basMain.empty()) found.language = Language::ElfC, found.main = cMain;
  else if (!basMain.empty() && asmMain.empty()) found.language = Language::BatariBasic, found.main = basMain;
  else found.language = Language::Dasm, found.main = asmMain.empty() ? (basMain.empty() ? cMain : basMain) : asmMain;
  // the ROM: the first word of the build file that names a .bin, .a26 or .elf
  std::string text = readAll(directory / script);
  static const std::regex rom(R"(([A-Za-z0-9_./\-]+)\.(bin|a26|elf)\b)");
  for (std::sregex_iterator match(text.begin(), text.end(), rom), matchEnd; match != matchEnd; ++match) {
    std::string romName = (*match)[0];
    size_t position = (size_t)match->position(0);
    if (position > 0 && (text[position - 1] == ')' || text[position - 1] == '}' || text[position - 1] == '%')) continue;  // $(NAME).bin, %.bin
    if (romName.rfind("-o", 0) == 0) romName.erase(0, 2);  // dasm -oout/game.bin
    if (romName.empty() || romName[0] == '-') continue;
    found.output = romName;
    break;
  }
  if (found.output == Project().output) found.output = fs::path(found.main).stem().string() + ".bin";
  *result = found;
  return true;
}

const std::vector<std::string>& tvStandardIds() {
  static const std::vector<std::string> ids = {"NTSC", "PAL", "PAL60", "SECAM", "NTSC50", "SECAM60", "PAL-M", "AUTO"};
  return ids;
}

std::string tvLabel(const std::string& tv) { return tv == "AUTO" ? std::string(T("Multiple (in the code)")) : tv; }

std::vector<std::string> tvStandardsFor(Language language) {
  if (language == Language::BatariBasic || language == Language::ElfC) return {"NTSC", "PAL", "PAL60"};
  std::vector<std::string> result;
  for (auto& standard : tvStandardIds())
    if (standard != "AUTO") result.push_back(standard);
  return result;
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

std::vector<std::string> Project::runArgs(Toolchain& toolchain, bool debug, std::string* error) const {
  std::string rom = outputPath();
  if (!fs::exists(rom)) {
    if (error) *error = F("The ROM does not exist yet: build first ({0}).", {rom});
    return {};
  }
  Tool* tool = toolchain.get(emulator);
  if (!tool || !tool->found()) {
    if (error) *error = F("{0} not found. Install it or set its path in Settings > Tools.", {tool ? tool->name : emulator});
    return {};
  }
  std::vector<std::string> arguments = {tool->path};
  if (emulator == "stella") {
    if (debug) arguments.push_back("-debug");
    if (!format.empty() && format != "AUTO") arguments.insert(arguments.end(), {"-bs", stellaFormat(format)});
    if (!tv.empty() && tv != "AUTO" && tvSupported(emulator, tv)) arguments.insert(arguments.end(), {"-format", tv});
  } else if (emulator == "gopher2600") {
    arguments.push_back(debug ? "DEBUG" : "RUN");
    if (!format.empty() && format != "AUTO") arguments.insert(arguments.end(), {"-mapping", format});
    if (!tv.empty() && tv != "AUTO" && tvSupported(emulator, tv)) arguments.insert(arguments.end(), {"-tv", tv});
  }
  arguments.push_back(rom);
  return arguments;
}

static std::string readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

static void replaceAll(std::string& text, const std::string& search, const std::string& replacement) {
  for (size_t pos = 0; (pos = text.find(search, pos)) != std::string::npos; pos += replacement.size()) text.replace(pos, search.size(), replacement);
}

std::vector<std::pair<std::string, const char*>> Project::examples(Language language) {
  if (language == Language::Dasm || language == Language::ElfC) return {{"animation", N_("Example: an animated sprite")}};
  return {};
}

// A .vcsgfx of an example comes with the tables its program includes (.gfx.asm, or .gfx.h
// for C), written as the graphics editor writes them on save.
static void writeGfxIncludes(const fs::path& dir, bool isC) {
  std::error_code errorCode;
  for (auto& entry : fs::directory_iterator(dir, errorCode)) {
    if (entry.path().extension() != ".vcsgfx") continue;
    gfx::Asset asset;
    std::string error;
    if (!gfx::fromJson(readFile(entry.path().string()), &asset, &error)) continue;
    std::string sourceName = entry.path().filename().string();
    std::ofstream(dir / (entry.path().stem().string() + (isC ? ".gfx.h" : ".gfx.asm")), std::ios::binary)
        << (isC ? gfx::cInclude(asset, sourceName) : gfx::dasmInclude(asset, sourceName));
  }
}

bool Project::create(const std::string& dirIn, const std::string& projName, Language projectLanguage, const std::string& tvStandard,
                     Project* result, std::string* error, const std::string& example) {
  fs::path folder(dirIn);
  std::error_code errorCode;
  fs::create_directories(folder, errorCode);
  if (errorCode) {
    if (error) *error = F("Cannot create {0}: {1}", {folder.string(), errorCode.message()});
    return false;
  }
  Project newProject;
  newProject.dir = fs::absolute(folder).string();
  newProject.file = (fs::path(newProject.dir) / (projName + ".vcsproj")).string();
  newProject.name = projName;
  newProject.language = projectLanguage;
  newProject.tv = tvStandard;
  newProject.output = "build/" + projName + ".bin";
  if (fs::exists(newProject.file)) {
    if (error) *error = F("{0} already exists.", {newProject.file});
    return false;
  }
  if (projectLanguage == Language::Dasm) {
    fs::path templateDir = fs::path(dataDir()) / "templates" / "dasm";
    fs::path exampleDir = example.empty() ? templateDir : fs::path(dataDir()) / "templates" / ("dasm-" + example);
    std::string asmText = readFile((exampleDir / "main.asm").string());
    if (asmText.empty()) {
      if (error) *error = F("Template not found in {0}", {exampleDir.string()});
      return false;
    }
    bool pal = tvIs50Hz(tvStandard);
    const char* lines50 = "VBLANK_LINES   = 45\nVISIBLE_LINES  = 228\nOVERSCAN_LINES = 36";
    const char* lines60 = "VBLANK_LINES   = 37\nVISIBLE_LINES  = 192\nOVERSCAN_LINES = 30";
    std::string defs = pal ? lines50 : lines60, tvText = tvStandard + " timing";
    if (tvStandard == "AUTO") {  // several systems: the program chooses when it is assembled
      defs = std::string("        IFCONST PAL     ; 50 Hz: assemble with -DPAL=1 (Project settings > DASM arguments)\n") +
             lines50 + "\n        ELSE\n" + lines60 + "\n        ENDIF";
      tvText = "NTSC timing, or PAL assembled with -DPAL=1";
    }
    replaceAll(asmText, "{{NAME}}", projName);
    replaceAll(asmText, "{{TV}} timing", tvText);
    replaceAll(asmText, "{{TVDEFS}}", defs);
    fs::path mainFile = fs::path(newProject.dir) / "main.asm";
    if (!fs::exists(mainFile)) std::ofstream(mainFile) << asmText;
    fs::path headerFile = fs::path(newProject.dir) / "2600.h";
    if (!fs::exists(headerFile)) fs::copy_file(templateDir / "2600.h", headerFile, errorCode);
    // the example's graphics, and the tables they give
    for (auto& entry : fs::directory_iterator(exampleDir, errorCode))
      if (entry.path().extension() == ".vcsgfx" && !fs::exists(fs::path(newProject.dir) / entry.path().filename()))
        fs::copy_file(entry.path(), fs::path(newProject.dir) / entry.path().filename(), errorCode);
    writeGfxIncludes(newProject.dir, false);
    newProject.main = "main.asm";
  } else if (projectLanguage == Language::BatariBasic) {
    newProject.main = "main.bas";
    newProject.output = "main.bas.bin";
    newProject.format = "AUTO";  // the game says it (set romsize, set kernel); Stella and Gopher2600 detect it
    fs::path templateDir = fs::path(dataDir()) / "templates" / "bb" / "main.bas";
    std::string text = readFile(templateDir.string());
    if (text.empty()) {
      if (error) *error = F("Template not found in {0}", {templateDir.string()});
      return false;
    }
    replaceAll(text, "{{NAME}}", projName);
    // batari Basic times the frame for NTSC unless told otherwise (PAL60 keeps NTSC timing)
    if (tvIs50Hz(tvStandard)) replaceAll(text, "  set romsize 4k\n", "  set romsize 4k\n  set tv pal\n");
    fs::path mainFile = fs::path(newProject.dir) / "main.bas";
    if (!fs::exists(mainFile)) std::ofstream(mainFile, std::ios::binary) << text;
  } else if (projectLanguage == Language::ElfC) {
    if (projName.find_first_of(" \t") != std::string::npos) {
      if (error) *error = T("ELF projects build with make, which does not take spaces in file names: choose a name without spaces.");
      return false;
    }
    // data/templates/elf, copied whole, with an example's files over it; {{NAME}} in the
    // Makefile and main.c
    fs::path templateDir = fs::path(dataDir()) / "templates" / "elf";
    fs::path exampleDir = fs::path(dataDir()) / "templates" / ("elf-" + example);
    if (!fs::exists(templateDir / "Makefile") || (!example.empty() && !fs::exists(exampleDir / "src" / "main.c"))) {
      if (error) *error = F("Template not found in {0}", {(example.empty() ? templateDir : exampleDir).string()});
      return false;
    }
    std::vector<fs::path> from{templateDir};
    if (!example.empty()) from.insert(from.begin(), exampleDir);  // the first copy of a file wins
    for (auto& sourceDir : from)
      for (auto& entry : fs::recursive_directory_iterator(sourceDir, errorCode)) {
        if (!entry.is_regular_file()) continue;
        fs::path relativePath = fs::relative(entry.path(), sourceDir), destination = fs::path(newProject.dir) / relativePath;
        if (relativePath == "LICENSE.md" || fs::exists(destination)) continue;
        fs::create_directories(destination.parent_path(), errorCode);
        std::string text = readFile(entry.path().string());
        replaceAll(text, "{{NAME}}", projName);
        std::ofstream(destination, std::ios::binary) << text;
      }
    writeGfxIncludes(fs::path(newProject.dir) / "src", true);
    newProject.main = "src/main.c";
    newProject.buildCommand = "make";
    newProject.format = "ELF";
  }
  if (!newProject.save(error)) return false;
  if (result) *result = newProject;
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
