// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "project.h"

#include <filesystem>
#include <algorithm>
#include <set>
#include <functional>
#include <regex>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "gfx.h"
#include "music/export.h"
#include "music/tuning.h"
#include "i18n.h"
#include "paths.h"
#include "toolchain.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace vcs {

const char* languageId(Language language) {
  switch (language) {
    case Language::Dasm: return "dasm";
    case Language::BatariBasic: return "batari-basic";
    case Language::ElfC: return "elf-c";
    case Language::AceC: return "ace-c";
    default: return "unknown";
  }
}
Language languageFromId(const std::string& id) {
  if (id == "dasm") return Language::Dasm;
  if (id == "batari-basic") return Language::BatariBasic;
  if (id == "elf-c") return Language::ElfC;
  if (id == "ace-c") return Language::AceC;
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
                                   "stack_reserve", "elf_cartridge", "elf_flash_kb", "elf_ram_kb", "emulator_options",
                                   "emulator_args", "source_dir", "resources_dir", "generated_dir", "build_dir"};

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
    parsed.sourceDir = document.value("source_dir", "");
    parsed.resourcesDir = document.value("resources_dir", "");
    parsed.generatedDir = document.value("generated_dir", "");
    parsed.buildDir = document.value("build_dir", "");
    if (document.contains("tool_paths") && document["tool_paths"].is_object())
      for (auto& [key, value] : document["tool_paths"].items())
        if (value.is_string() && !value.get<std::string>().empty()) parsed.toolPaths[key] = value.get<std::string>();
    parsed.buildFlags = document.value("build_flags", "");
    parsed.spaceWarnPercent = document.value("space_warn_percent", 90);
    parsed.stackReserve = document.value("stack_reserve", 8);
    parsed.elfCartridge = document.value("elf_cartridge", parsed.elfCartridge) == "pluscart" ? "pluscart" : "chameleon";
    parsed.elfFlashKb = std::max(0, document.value("elf_flash_kb", 0));
    parsed.elfRamKb = std::max(0, document.value("elf_ram_kb", 0));
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
  for (auto [key, value] : {std::pair{"source_dir", &sourceDir}, {"resources_dir", &resourcesDir}, {"generated_dir", &generatedDir}, {"build_dir", &buildDir}})
    if (!value->empty()) document[key] = *value;
  json toolPathsJson = json::object();
  for (auto& [key, value] : toolPaths)
    if (!value.empty()) toolPathsJson[key] = value;
  if (!toolPathsJson.empty()) document["tool_paths"] = toolPathsJson;
  if (!buildFlags.empty()) document["build_flags"] = buildFlags;
  if (spaceWarnPercent != 90) document["space_warn_percent"] = spaceWarnPercent;
  if (stackReserve != 8) document["stack_reserve"] = stackReserve;
  if (language == Language::ElfC) {
    document["elf_cartridge"] = elfCartridge;
    if (elfFlashKb > 0 && elfCartridge == "chameleon") document["elf_flash_kb"] = elfFlashKb;
    if (elfRamKb > 0) document["elf_ram_kb"] = elfRamKb;
  }
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

std::string Project::generatedFolder(const std::string& sourcePath, const std::string& kind) const {
  std::error_code errorCode;
  fs::path sourceFolder = fs::weakly_canonical(fs::path(sourcePath).parent_path(), errorCode);
  fs::path projectFolder = fs::weakly_canonical(dir, errorCode);
  auto inside = [](const fs::path& relative) { return !relative.empty() && *relative.begin() != ".."; };
  if (generatedDir.empty() || dir.empty() || !inside(sourceFolder.lexically_relative(projectFolder))) return fs::path(sourcePath).parent_path().string();
  fs::path folder = fs::path(abs(generatedDir)) / kind;
  // resources/music/level1/a.vcsmusic exports to generated/music/level1/music.asm
  fs::path relative = sourceFolder.lexically_relative(fs::weakly_canonical(abs(resourcesDir.empty() ? "resources" : resourcesDir), errorCode) / kind);
  if (inside(relative) && relative != ".") folder /= relative;
  return folder.lexically_normal().string();
}

std::string Project::buildFolder() const { return buildDir.empty() ? fs::path(outputPath()).parent_path().string() : abs(buildDir); }

std::vector<std::string> Project::layoutIncludeDirs() const {
  std::vector<std::string> folders;
  if (!sourceDir.empty()) folders.push_back(abs(sourceDir));
  if (!generatedDir.empty()) folders.push_back(abs(generatedDir + "/gfx")), folders.push_back(abs(generatedDir + "/music"));
  return folders;
}

std::string Project::basicOutput(const std::string& suffix) const {
  if (buildDir.empty()) return basicWritten(suffix);
  return (fs::path(buildFolder()) / (fs::path(main).filename().string() + suffix)).string();
}

void Project::collectBasicOutputs() const {
  if (buildDir.empty()) return;
  std::error_code errorCode;
  fs::path to = buildFolder();
  fs::create_directories(to, errorCode);
  for (auto suffix : {".bin", ".asm", ".list.txt", ".symbol.txt", ".lst", ".sym", "-cc.elf"})
    if (fs::exists(basicWritten(suffix), errorCode)) fs::rename(basicWritten(suffix), basicOutput(suffix), errorCode);
  // what batari Basic leaves where it runs (the project's folder) on the way
  for (auto name : {"bB.asm", "includes.bB", "2600basic_variable_redefs.h"})
    if (fs::exists(fs::path(dir) / name, errorCode)) fs::rename(fs::path(dir) / name, to / name, errorCode);
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
  for (auto& folder : layoutIncludeDirs()) arguments.push_back("-I" + folder);
  for (auto& includeDir : includeDirs) arguments.push_back("-I" + abs(includeDir));
  for (auto& flag : splitCommand(buildFlags))
    if (flag.rfind("-D", 0) == 0 || flag.rfind("-M", 0) == 0 || flag.rfind("-I", 0) == 0) arguments.push_back(flag);  // what changes the code
  return arguments;
}

std::vector<std::string> Project::buildTools(Toolchain& toolchain) const {
  if (!buildCommand.empty()) {
    if (!isC(language)) return {};
    std::vector<std::string> tools = {"arm-gcc"};
    auto words = splitCommand(buildCommand);
    if (!words.empty() && fs::path(words[0]).stem().string().find("make") != std::string::npos) tools.push_back("make");
    return tools;
  }
  switch (language) {
    case Language::Dasm: return toolPaths.count("dasm") && !toolPath(toolchain, "dasm").empty() ? std::vector<std::string>{} : std::vector<std::string>{"dasm"};
    case Language::BatariBasic: {
      // 1.9 is WebAssembly (2600basic.wasm next to its script) and runs on wasmtime
      std::string bb = toolPath(toolchain, basicTool());
      bool hasOwnBb = toolPaths.count("bb") && !bb.empty();
      std::error_code errorCode;
      std::vector<std::string> tools;
      if (!hasOwnBb) tools.push_back(basicTool());
      if (!pxeSource().empty()) tools.push_back("arm-gcc");  // C linked into the PXE kernel
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
      // the project's folders, for a Makefile that takes them (SRC_DIR ?= src)
      for (auto [variable, value] : {std::pair{"SRC_DIR", &sourceDir}, {"GENERATED_DIR", &generatedDir}, {"BUILD_DIR", &buildDir}})
        if (!value->empty()) env->push_back({variable, *value});
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
      for (auto& folder : layoutIncludeDirs()) arguments.push_back("-I" + folder);
      for (auto& includeDir : includeDirs) arguments.push_back("-I" + abs(includeDir));
      for (auto& flag : splitCommand(buildFlags)) arguments.push_back(flag);
      return arguments;
    }
    case Language::BatariBasic: {
      std::string bbPath = toolPath(toolchain, basicTool());
      if (bbPath.empty()) {
        if (error) *error = toolPaths.count("bb") ? F("The project's batari Basic ({0}) does not exist.", {toolPaths.at("bb")})
                            : basicTool() == "bb-pxe" ? std::string(T("The PXE beta of batari Basic not found. Unpack it into the bb-pxe folder of VCSIDE's tools folder or set its path in Settings > Tools."))
                                                  : std::string(T("batari Basic not found. Set the path to 2600basic.sh / 2600bas.bat in Settings > Tools."));
        return {};
      }
      // Its script runs in the source's folder (1.9 only lets wasmtime see "."), takes the
      // file name, and finds its own files through the bB variable. wasmtime has to be in
      // PATH: an app started from the desktop may not have the shell's PATH.
      // A project with a generated folder runs it in its own folder instead, with the source's
      // path, so that the program's asm blocks can include generated/music/music.asm.
      fs::path source = abs(main);
      const bool fromProject = !generatedDir.empty();
      if (workDir) *workDir = fromProject ? dir : source.parent_path().string();
      const std::string sourceArgument = fromProject ? fs::path(main).lexically_normal().generic_string() : source.filename().string();
      if (env) {
        env->push_back({"bB", fs::path(bbPath).parent_path().string()});
        std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
        if (Tool* wasmtime = toolchain.get("wasmtime"); wasmtime && wasmtime->found()) path = fs::path(wasmtime->path).parent_path().string() + pathSeparator + path;
        env->push_back({"PATH", path});
      }
#ifdef _WIN32
      return {"cmd.exe", "/c", bbPath, sourceArgument};
#else
      return {"/bin/sh", bbPath, sourceArgument};
#endif
    }
    case Language::ElfC:
    case Language::AceC:
      if (error) *error = T("C projects build with their Makefile: set \"build_command\" (e.g. \"make\") in the project.");
      return {};
    default:
      if (error) *error = T("Unknown project language.");
      return {};
  }
}

std::string Project::pxeSource() const {
  std::error_code errorCode;
  if (!pxe()) return "";
  fs::path source = fs::path(abs(main)).parent_path() / "pxe.c";
  return fs::is_regular_file(source, errorCode) ? source.string() : "";
}

std::vector<std::string> Project::pxeLinkArgs(Toolchain& toolchain, std::string* error) const {
  std::string source = pxeSource();
  if (source.empty()) return {};
  std::string gcc = toolPath(toolchain, "arm-gcc");
  if (gcc.empty()) {
    if (error) *error = T("pxe.c needs the Arm GNU toolchain (arm-none-eabi-gcc): install it or set its path in Settings > Tools.");
    return {};
  }
  // one relocatable ELF, as the PXE kernel's: Cortex-M0 code (it runs on every ELF cart),
  // long calls (vcsLib may be far away), and the kernel's call to vcsEndOverblank sent to
  // __wrap_vcsEndOverblank, which calls __real_vcsEndOverblank
  return {gcc, "-mcpu=cortex-m0", "-mthumb", "-O2", "-std=c11", "-Wall", "-ffreestanding", "-fno-builtin", "-mlong-calls",
          "-nostdlib", "-r", "-Wl,--wrap=vcsEndOverblank", "-I" + fs::path(source).parent_path().string(), "-I" + generatedFolder(source, "music"),
          "-o", basicOutput(".linked"), source, basicOutput(".bin")};
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
  // a folder with a project file of its own is opened as that project, not detected again
  for (auto& entry : fs::directory_iterator(directory, errorCode))
    if (entry.path().extension() == ".vcsproj") return false;
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
  Project found;
  found.dir = directory.string();
  found.name = directory.filename().string();
  found.file = (directory / (found.name + ".vcsproj")).string();
  found.buildCommand = command;
  // the main source, in the folder or in src/: main.* first, else a file with the 6502's
  // processor line (assembly), else any (batari Basic, C)
  std::string asmMain, basMain, cMain;
  for (const fs::path& folder : {directory, directory / "src"}) {
    std::vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(folder, errorCode))
      if (entry.is_regular_file(errorCode)) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (auto& file : files) {
      std::string ext = file.extension().string(), stem = file.stem().string();
      for (auto& character : ext) character = (char)std::tolower((unsigned char)character);
      std::string relative = fs::relative(file, directory, errorCode).generic_string();
      bool isMain = stem == "main";
      if (ext == ".asm" || ext == ".s" || ext == ".a") {
        bool hasProcessor = readAll(file).find("processor") != std::string::npos;
        if ((isMain && (hasProcessor || !command.empty())) || (asmMain.empty() && hasProcessor)) asmMain = relative;
      } else if (ext == ".bas" && (isMain || basMain.empty())) {
        basMain = relative;
      } else if (ext == ".c" && (isMain || cMain.empty())) {
        cMain = relative;
      }
    }
    if (!asmMain.empty() || !basMain.empty() || !cMain.empty()) break;  // the folder's own sources win over src/
  }
  if (!cMain.empty() && asmMain.empty() && basMain.empty()) found.language = Language::ElfC, found.main = cMain;
  else if (!basMain.empty() && asmMain.empty()) found.language = Language::BatariBasic, found.main = basMain;
  else found.language = Language::Dasm, found.main = asmMain.empty() ? (basMain.empty() ? cMain : basMain) : asmMain;

  if (command.empty()) {
    // no build of its own: VCSIDE builds it with DASM or batari Basic (C needs a Makefile)
    if (isC(found.language) || found.main.empty()) return false;
    found.format = "AUTO";  // the emulators tell the format from the ROM
    if (found.language == Language::Dasm) {
      found.output = "build/" + fs::path(found.main).stem().string() + ".bin";
      // DASM looks for include files where it runs: the source's folder and include/ too
      std::string sourceFolder = fs::path(found.main).parent_path().generic_string();
      if (!sourceFolder.empty()) found.includeDirs.push_back(sourceFolder);
      if (fs::is_directory(directory / "include", errorCode)) found.includeDirs.push_back("include");
    } else {
      found.output = found.main + ".bin";  // batari Basic writes it next to the source
    }
    *result = found;
    return true;
  }
  // the ROM: the first word of the build file that names a .bin, .a26, .elf or .ace
  std::string text = readAll(directory / script);
  static const std::regex rom(R"(([A-Za-z0-9_./\-]+)\.(bin|a26|elf|ace)\b)");
  for (std::sregex_iterator match(text.begin(), text.end(), rom), matchEnd; match != matchEnd; ++match) {
    std::string romName = (*match)[0];
    size_t position = (size_t)match->position(0);
    if (position > 0 && (text[position - 1] == ')' || text[position - 1] == '}' || text[position - 1] == '%')) continue;  // $(NAME).bin, %.bin
    if (romName.rfind("-o", 0) == 0) romName.erase(0, 2);  // dasm -oout/game.bin
    if (romName.empty() || romName[0] == '-') continue;
    found.output = romName;
    break;
  }
  if (fs::path(found.output).extension() == ".ace") {  // only Gopher2600 runs ACE
    found.format = "ACE", found.emulator = "gopher2600";
    if (found.language == Language::ElfC) found.language = Language::AceC;
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
  if (language == Language::BatariBasic || isC(language)) return {"NTSC", "PAL", "PAL60"};
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
  if (!formatSupported(emulator, format)) {
    if (error) *error = F("{0} cannot run {1} cartridges: choose Gopher2600 in Build > Emulator.", {tool->name, format});
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

// A template's shared.json names the files it takes from elsewhere in data/templates, so
// that the repository keeps one copy of each: {"<path in the project>": {"from": "<path in
// data/templates>", "change": {...}}}. "change" (JSON files only) is merged over the file
// (RFC 7386: an object's keys are replaced one by one, arrays whole), written as VCSIDE's
// editors save. The text substitutions are applied to every file. A file the project
// already has is kept.
static bool copySharedFiles(const fs::path& templateDir, const fs::path& projectDir,
                            const std::vector<std::pair<std::string, std::string>>& substitutions, std::string* error,
                            const std::set<std::string>& leaveOut = {}) {
  fs::path manifestFile = templateDir / "shared.json";
  if (!fs::exists(manifestFile)) return true;
  json manifest = json::parse(readFile(manifestFile.string()), nullptr, false);
  if (!manifest.is_object()) {
    if (error) *error = F("Template not found in {0}", {manifestFile.string()});
    return false;
  }
  fs::path templatesDir = fs::path(dataDir()) / "templates";
  std::error_code errorCode;
  for (auto& [destinationName, entry] : manifest.items()) {
    fs::path destination = projectDir / destinationName, source = templatesDir / entry.value("from", std::string());
    if (fs::exists(destination) || leaveOut.count(destinationName)) continue;
    std::string text = readFile(source.string());
    if (text.empty()) {
      if (error) *error = F("Template not found in {0}", {source.string()});
      return false;
    }
    if (entry.contains("change")) {
      json document = json::parse(text, nullptr, false);
      if (document.is_discarded()) {
        if (error) *error = F("Template not found in {0}", {source.string()});
        return false;
      }
      document.merge_patch(entry["change"]);
      text = document.dump(1) + "\n";
    }
    for (auto& [search, replacement] : substitutions) replaceAll(text, search, replacement);
    fs::create_directories(destination.parent_path(), errorCode);
    std::ofstream(destination, std::ios::binary) << text;
  }
  return true;
}

// VCSIDE.md: the project's notes for the assistant (kernel rules, where things live, how it
// builds, what not to touch), the example's own or else the template's, with the name in it.
// C templates are copied whole, so theirs comes with the rest.
static void copyProjectNotes(const std::vector<fs::path>& from, const fs::path& projectDir, const std::string& projName,
                             const std::vector<std::pair<std::string, std::string>>& substitutions = {}) {
  fs::path destination = projectDir / "VCSIDE.md";
  if (fs::exists(destination)) return;
  for (const fs::path& dir : from) {
    std::string text = readFile((dir / "VCSIDE.md").string());
    if (text.empty()) continue;
    replaceAll(text, "{{NAME}}", projName);
    for (auto& [search, replacement] : substitutions) replaceAll(text, search, replacement);
    std::ofstream(destination, std::ios::binary) << text;
    return;
  }
}

// The DASM examples: the folder in data/templates, the cartridges each is made for, and
// for VCSIDE The Game (dasm-game), its switches TITLE_SCREEN and MUSIC.
struct DasmExample {
  const char* id;
  const char* name;  // for people, to translate
  const char* folder;
  std::vector<std::string> formats;
  int titleScreen = 1, music = 1;
};
static const std::vector<DasmExample>& dasmExamples() {
  static const std::vector<DasmExample> list = {
      {"game", N_("VCSIDE The Game"), "dasm-game", {"4K", "F8", "F6", "F4"}},
      {"game-notitle", N_("VCSIDE The Game, without the title screen"), "dasm-game", {"4K", "F8", "F6", "F4"}, 0, 1},
      {"game-2k", N_("VCSIDE The Game, without the title screen or the song"), "dasm-game", {"2K"}, 0, 0},
      {"game-dpcplus", N_("VCSIDE The Game"), "dasm-dpcplus-game", {"DPC+"}},
      {"game-cdfj", N_("VCSIDE The Game"), "dasm-cdfj-game", {"CDFJ"}},
      {"animation", N_("Example: an animated sprite"), "dasm-animation", {"4K"}},
      {"dpcplus", N_("Example: music on a DPC+ cartridge"), "dasm-dpcplus", {"DPC+"}},
      {"cdfj", N_("Example: C and music on a CDFJ cartridge"), "dasm-cdfj", {"CDFJ"}}};
  return list;
}
static const DasmExample* findDasmExample(const std::string& id) {
  for (auto& entry : dasmExamples())
    if (id == entry.id) return &entry;
  return nullptr;
}

// Banks of 4K and the first hotspot (bank 0's) of the banked DASM template's formats.
struct BankedFormat {
  const char* id;
  int banks;
  const char* hotspot;
};
static const BankedFormat kBankedFormats[] = {{"F8", 2, "$FFF8"}, {"F6", 4, "$FFF6"}, {"F4", 8, "$FFF4"}};

// batari Basic's set romsize for each format (2k does not hold the game and its music).
static const std::pair<const char*, const char*> kBasicRomSizes[] = {{"4K", "4k"}, {"F8", "8k"}, {"F6", "16k"}, {"F4", "32k"}};

// The batari Basic examples, for every cartridge: the folder in data/templates and, for
// VCSIDE The Game (bb-game), whether it has the title screen.
struct BasicExample {
  const char* id;
  const char* name;  // for people, to translate
  const char* folder;
  bool titleScreen = true;
};
static const BasicExample kBasicExamples[] = {
    {"game", N_("VCSIDE The Game"), "bb-game"},
    {"game-notitle", N_("VCSIDE The Game, without the title screen"), "bb-game", false},
    {"square", N_("Example: a square in a room, with music"), "bb-square"}};
// PXE (batari Basic's ELF kernel) has a program of its own: no banks, 17 sprites, its colours.
static const BasicExample kPxeExamples[] = {
    {"game", N_("VCSIDE The Game"), "bb-pxe-game"},
    {"game-notitle", N_("VCSIDE The Game, without the title screen"), "bb-pxe-game", false}};

// A template's optional parts: the lines between {{TAG}} and {{/TAG}} (each on a line of its
// own) stay without the two markers, or go with them.
static void keepSection(std::string& text, const std::string& tag, bool keep) {
  const std::string open = "{{" + tag + "}}\n", close = "{{/" + tag + "}}\n";
  for (size_t start; (start = text.find(open)) != std::string::npos;) {
    size_t end = text.find(close, start);
    if (end == std::string::npos) break;
    if (keep) text.erase(end, close.size()), text.erase(start, open.size());
    else text.erase(start, end + close.size() - start);
  }
}

// The PAL colour that looks closest to an NTSC one (by RGB distance).
static int palColourFor(int ntscColour) {
  uint32_t wanted = gfx::rgb(gfx::Tv::NTSC, ntscColour);
  int best = 0;
  long bestDistance = -1;
  for (int candidate = 0; candidate < 256; candidate += 2) {
    uint32_t have = gfx::rgb(gfx::Tv::PAL, candidate);
    long distance = 0;
    for (int shift : {0, 8, 16}) {
      long difference = (long)((wanted >> shift) & 0xFF) - (long)((have >> shift) & 0xFF);
      distance += difference * difference;
    }
    if (bestDistance < 0 || distance < bestDistance) bestDistance = distance, best = candidate;
  }
  return best;
}

// The template's graphics are drawn with NTSC colours. A batari Basic program writes them
// as they are, so a PAL (or PAL60) project gets .vcsgfx files with the closest PAL ones.
static void graphicsToPal(const fs::path& dir) {
  std::error_code errorCode;
  for (auto& entry : fs::directory_iterator(dir, errorCode)) {
    if (entry.path().extension() != ".vcsgfx") continue;
    json document = json::parse(readFile(entry.path().string()), nullptr, false);
    if (document.is_discarded()) continue;
    std::function<void(json&)> convert = [&](json& node) {
      if (node.is_array()) {
        for (auto& item : node) convert(item);
      } else if (node.is_object()) {
        for (auto& [key, value] : node.items()) {
          if ((key == "pf" || key == "bk") && value.is_number_integer() && value.get<int>() >= 0) value = palColourFor(value.get<int>());
          else if (key == "colours" && value.is_array()) {
            for (auto& colour : value)
              if (colour.is_number_integer() && colour.get<int>() >= 0) colour = palColourFor(colour.get<int>());
          } else convert(value);
        }
      }
    };
    convert(document);
    std::ofstream(entry.path(), std::ios::binary) << document.dump(1) << "\n";
  }
}

// The `rem vcsgfx` regions of a batari Basic program, filled from the project's .vcsgfx
// files as the graphics editor fills them on save, for the kernel the program sets.
static void fillBasicRegions(const fs::path& program, const fs::path& gfxDir) {
  std::string text = readFile(program.string());
  if (text.find("rem vcsgfx") == std::string::npos) return;
  gfx::Profile profile = gfx::profileFromBasic(text);
  std::error_code errorCode;
  for (auto& entry : fs::directory_iterator(gfxDir, errorCode)) {
    if (entry.path().extension() != ".vcsgfx") continue;
    gfx::Asset asset;
    if (!gfx::fromJson(readFile(entry.path().string()), &asset, nullptr)) continue;
    text = gfx::updateBasicRegions(text, entry.path().filename().string(), asset, profile, nullptr, nullptr);
  }
  std::ofstream(program, std::ios::binary) << text;
}

std::vector<std::string> Project::cartridgeFormats(Language language) {
  if (language == Language::Dasm) return {"2K", "4K", "F8", "F6", "F4", "DPC+", "CDFJ"};
  if (language == Language::BatariBasic) return {"4K", "F8", "F6", "F4", "PXE"};
  return {};
}

// Every language and cartridge has one today (DPC+ and CDFJ: dasm-dpcplus-empty and
// dasm-cdfj-empty); a new cartridge without one would say so here.
bool Project::hasEmptyTemplate(Language, const std::string&) { return true; }

std::vector<std::pair<std::string, const char*>> Project::examples(Language language, const std::string& format) {
  if (language == Language::Dasm) {
    std::vector<std::pair<std::string, const char*>> forFormat;
    for (auto& entry : dasmExamples())
      if (format.empty() || std::find(entry.formats.begin(), entry.formats.end(), format) != entry.formats.end()) forFormat.push_back({entry.id, entry.name});
    return forFormat;
  }
  if (language == Language::BatariBasic) {
    std::vector<std::pair<std::string, const char*>> all;
    if (format == "PXE")
      for (auto& entry : kPxeExamples) all.push_back({entry.id, entry.name});
    else
      for (auto& entry : kBasicExamples) all.push_back({entry.id, entry.name});
    return all;
  }
  if (language == Language::ElfC) return {{"game", N_("VCSIDE The Game")}, {"animation", N_("Example: an animated sprite")}};
  if (language == Language::AceC)
    return {{"game", N_("VCSIDE The Game")},
            {"animation", N_("Example: an animated sprite")},
            {"vcslib", N_("Example: the ELF game on vcsLib (also builds as ELF)")}};
  return {};
}

// DPC+'s ARM driver, from the batari Basic VCSIDE installs (or $bB's): VCSIDE does not
// carry it, the project gets a copy. Empty when there is none.
static std::string findDpcPlusDriver() {
  std::vector<fs::path> places = {fs::path(Toolchain::toolsDir()) / "bb" / "includes" / "DPCplus.arm"};
  if (const char* bb = std::getenv("bB")) places.push_back(fs::path(bb) / "includes" / "DPCplus.arm");
  for (auto& place : places)
    if (fs::exists(place)) return place.string();
  return "";
}

std::string Project::dpcPlusDriver() { return findDpcPlusDriver(); }

// What is checked is what the templates rely on, not the driver itself (VCSIDE carries none
// of it): its size, "CDFJ" three times where the driver keeps its signature ($0174, which
// emulators look for to tell CDFJ from CDF and CDFJ+), and an entry point at each of $0750,
// $0754, $0758 and $075C (the code calls setNote, resetWave, getWavePtr and setWaveSize there).
bool Project::checkCdfjDriver(const std::string& path, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = F("Cannot read {0}", {path});
    return false;
  }
  std::string bytes((std::istreambuf_iterator<char>(in)), {});
  if (bytes.size() != 2048) {
    if (error) *error = F("{0} has {1} bytes: CDFJ's driver has 2048.", {fs::path(path).filename().string(), std::to_string(bytes.size())});
    return false;
  }
  if (bytes.compare(0x174, 12, "CDFJCDFJCDFJ") != 0) {
    if (error) *error = F("{0} is not CDFJ's driver: it lacks the CDFJ signature (a CDF or CDFJ+ driver, or another file?).", {fs::path(path).filename().string()});
    return false;
  }
  bool entryPoints = (unsigned char)bytes[0x753] == 0x47;  // each one ends in a Thumb "bx"
  for (int entry = 0x754; entry < 0x760; entry += 4) entryPoints = entryPoints && bytes.compare(entry, 4, bytes, 0x750, 4) == 0;
  if (!entryPoints) {
    if (error) *error = F("{0} is a CDFJ driver of another version: its functions are not where the template calls them ($0751-$075D).", {fs::path(path).filename().string()});
    return false;
  }
  return true;
}

bool Project::addCdfjDriver(const std::string& path, const std::string& projectDir, std::string* error) {
  if (!checkCdfjDriver(path, error)) return false;
  std::error_code errorCode;
  fs::path target = fs::path(projectDir) / kCdfjDriverFile;
  if (fs::exists(target, errorCode) && fs::equivalent(path, target, errorCode)) return true;
  fs::copy_file(path, target, fs::copy_options::overwrite_existing, errorCode);
  if (errorCode) {
    if (error) *error = F("Cannot write {0}", {target.string()});
    return false;
  }
  return true;
}

// A .vcsgfx of an example comes with the tables its program includes (.gfx.asm, or .gfx.h
// for C), written as the graphics editor writes them on save, from resources/gfx to
// generated/gfx.
static void writeGfxIncludes(const fs::path& dir, const fs::path& outputDir, bool isC) {
  std::error_code errorCode;
  fs::create_directories(outputDir, errorCode);
  for (auto& entry : fs::directory_iterator(dir, errorCode)) {
    if (entry.path().extension() != ".vcsgfx") continue;
    gfx::Asset asset;
    std::string error;
    if (!gfx::fromJson(readFile(entry.path().string()), &asset, &error)) continue;
    std::string sourceName = entry.path().filename().string();
    std::ofstream(outputDir / (entry.path().stem().string() + (isC ? ".gfx.h" : ".gfx.asm")), std::ios::binary)
        << (isC ? gfx::cInclude(asset, sourceName) : gfx::dasmInclude(asset, sourceName));
  }
}

// The .vcsmusic files of an example come with their export (music.asm, or music.h for C:
// every song of the folder with one player), written as the music editor writes it on
// save, tuned for the project's TV standard, from resources/music to generated/music.
static void writeMusicIncludes(const fs::path& dir, const fs::path& outputDir, const std::string& tvStandard) {
  std::error_code errorCode;
  std::set<std::string> written;
  for (auto& entry : fs::directory_iterator(dir, errorCode)) {
    if (entry.path().extension() != ".vcsmusic") continue;
    music::Song song;
    std::string error;
    music::ExportOptions options;
    if (!music::fromJson(readFile(entry.path().string()), &song, &error) || !music::exportTargetOf(song.exportTo, &options.target)) continue;
    if (!written.insert(music::exportFileName(options.target)).second) continue;
    std::string tv = tvStandard == "AUTO" ? "NTSC" : tvStandard;
    options.tv = tv;
    if (options.target == music::ExportTarget::BatariBasic) options.ramStart = song.exportRam;
    music::writeFolderExport(music::exportFolder(dir.string(), options.target, music::audioClockHz(tv), options, "", nullptr, outputDir.string()), &error);
  }
}

bool Project::create(const std::string& dirIn, const std::string& projName, Language projectLanguage, const std::string& tvStandard,
                     Project* result, std::string* error, const std::string& example, const std::string& format, const ElfBuilds& elfBuilds) {
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
  // the folders: programs, graphics and music, what their editors export, what the build writes
  newProject.sourceDir = "src", newProject.resourcesDir = "resources", newProject.generatedDir = "generated", newProject.buildDir = "build";
  const fs::path sourceFolder = fs::path(newProject.dir) / "src", gfxFolder = fs::path(newProject.dir) / "resources" / "gfx",
                 musicFolder = fs::path(newProject.dir) / "resources" / "music", generatedGfx = fs::path(newProject.dir) / "generated" / "gfx",
                 generatedMusic = fs::path(newProject.dir) / "generated" / "music";
  fs::create_directories(sourceFolder, errorCode);
  if (fs::exists(newProject.file)) {
    if (error) *error = F("{0} already exists.", {newProject.file});
    return false;
  }
  const auto formats = cartridgeFormats(projectLanguage);
  if (!format.empty() && !formats.empty() && std::find(formats.begin(), formats.end(), format) == formats.end()) {
    if (error) *error = F("There is no template for a {0} cartridge.", {format});
    return false;
  }
  if (projectLanguage == Language::Dasm) {
    // the cartridge: the chosen one, or the example's first (4K if neither)
    const DasmExample* dasmExample = example.empty() ? nullptr : findDasmExample(example);
    if (!example.empty() && !dasmExample) {
      if (error) *error = F("Template not found in {0}", {example});
      return false;
    }
    const std::string cartridge = !format.empty() ? format : dasmExample ? dasmExample->formats[0] : "4K";
    if (dasmExample && std::find(dasmExample->formats.begin(), dasmExample->formats.end(), cartridge) == dasmExample->formats.end()) {
      if (error) *error = F("That example is made for a {0} cartridge.", {dasmExample->formats[0]});
      return false;
    }
    if (example.empty() && !hasEmptyTemplate(projectLanguage, cartridge)) {
      if (error) *error = F("A {0} cartridge starts from its example.", {cartridge});
      return false;
    }
    const BankedFormat* banked = nullptr;
    for (auto& candidate : kBankedFormats)
      if (cartridge == candidate.id) banked = &candidate;
    fs::path templateDir = fs::path(dataDir()) / "templates" / "dasm";
    fs::path exampleDir = dasmExample            ? fs::path(dataDir()) / "templates" / dasmExample->folder
                          : banked                 ? fs::path(dataDir()) / "templates" / "dasm-banked"
                          : cartridge == "DPC+"    ? fs::path(dataDir()) / "templates" / "dasm-dpcplus-empty"
                          : cartridge == "CDFJ"    ? fs::path(dataDir()) / "templates" / "dasm-cdfj-empty"
                                                   : templateDir;
    newProject.format = cartridge;
    std::string asmText = readFile((exampleDir / "main.asm").string());
    if (asmText.empty()) {
      if (error) *error = F("Template not found in {0}", {exampleDir.string()});
      return false;
    }
    std::string dpcDriver;
    if (cartridge == "DPC+") {
      dpcDriver = findDpcPlusDriver();
      if (dpcDriver.empty()) {
        if (error) *error = T("A DPC+ project takes the cartridge's ARM driver (DPCplus.arm) from batari Basic: install batari Basic first (Tools).");
        return false;
      }
    }
    const bool cdfj = cartridge == "CDFJ";
    if (cdfj && projName.find_first_of(" \t") != std::string::npos) {
      if (error) *error = T("CDFJ projects build with make, which does not take spaces in file names: choose a name without spaces.");
      return false;
    }
    bool pal = tvIs50Hz(tvStandard);
    const char* lines50 = "VBLANK_LINES   = 45\nVISIBLE_LINES  = 228\nOVERSCAN_LINES = 36";
    const char* lines60 = "VBLANK_LINES   = 37\nVISIBLE_LINES  = 192\nOVERSCAN_LINES = 30";
    std::string defs = pal ? lines50 : lines60, tvText = tvStandard + " timing";
    if (tvStandard == "AUTO") {  // several systems: the program chooses when it is assembled
      defs = std::string("\tIFCONST PAL     ; 50 Hz: assemble with -DPAL=1 (Project settings > DASM arguments)\n") +
             lines50 + "\n\tELSE\n" + lines60 + "\n\tENDIF";
      tvText = cdfj ? "NTSC timing, or PAL with make DASMFLAGS=-DPAL=1" : "NTSC timing, or PAL assembled with -DPAL=1";
    }
    // the cartridge: the empty templates' size and banks
    const std::vector<std::pair<std::string, std::string>> cartridgeText = {
        {"{{FORMAT}}", cartridge},
        {"{{ROM_START}}", cartridge == "2K" ? "$F800" : "$F000"},
        {"{{ROM_BYTES}}", std::to_string(banked ? banked->banks * 4096 : cartridge == "2K" ? 2048 : 4096)},
        {"{{BANKS}}", banked ? std::to_string(banked->banks) : "1"},
        {"{{HOTSPOT}}", banked ? banked->hotspot : ""}};
    // VCSIDE The Game: its switches, and where its code starts and ends (bank 0 of a
    // banked cartridge, with the other banks after it)
    if (dasmExample && std::string(dasmExample->folder) == "dasm-game") {
      replaceAll(asmText, "{{TITLE_SCREEN}}", std::to_string(dasmExample->titleScreen));
      replaceAll(asmText, "{{MUSIC}}", std::to_string(dasmExample->music));
      replaceAll(asmText, "{{CODE_START}}", banked ? "\tORG $0000\n\tRORG $F000" : cartridge == "2K" ? "\tORG $F800" : "\tORG $F000");
      replaceAll(asmText, "{{CODE_END}}", banked ? readFile((exampleDir / "banks.asm").string()) : "\tORG $FFFC\n\t.word Reset\n\t.word Reset\n");
    }
    for (auto& [search, replacement] : cartridgeText) replaceAll(asmText, search, replacement);
    replaceAll(asmText, "{{NAME}}", projName);
    replaceAll(asmText, "{{TV}} timing", tvText);
    replaceAll(asmText, "{{TVDEFS}}", defs);
    // a program that chooses the TV system as it runs (the game example's menu) starts with
    // the project's: 0 NTSC, 1 PAL60, 2 PAL (50 Hz)
    replaceAll(asmText, "{{TV_SYSTEM}}", tvStandard == "PAL60" ? "1" : pal ? "2" : "0");
    replaceAll(asmText, "{{TV}}", tvStandard == "AUTO" ? "NTSC" : tvStandard);
    fs::path mainFile = sourceFolder / "main.asm";
    if (!fs::exists(mainFile)) std::ofstream(mainFile) << asmText;
    fs::path headerFile = sourceFolder / "2600.h";
    if (!fs::exists(headerFile)) fs::copy_file(templateDir / "2600.h", headerFile, errorCode);
    // the example's graphics and music (shared.json), and the tables they give
    std::set<std::string> leaveOut;  // what the game's switches leave unused
    if (dasmExample && !dasmExample->titleScreen) leaveOut.insert("resources/gfx/title.vcsgfx");
    if (dasmExample && !dasmExample->music) leaveOut.insert("resources/music/theme.vcsmusic");
    if (!copySharedFiles(exampleDir, newProject.dir, {{"{{NAME}}", projName}}, error, leaveOut)) return false;
    // CDFJ: the C code, its linker script and the Makefile that builds both (the driver
    // is not included: the Makefile says where it comes from); the templates take the
    // files they share from dasm-cdfj through their shared.json
    if (cdfj) {
      for (auto& entry : fs::directory_iterator(exampleDir, errorCode)) {
        std::string extension = entry.path().extension().string(), fileName = entry.path().filename().string();
        if (fileName != "Makefile" && extension != ".c" && extension != ".h" && extension != ".S" && extension != ".ld") continue;
        // the Makefile and the linker script in the project's folder, the code in src/
        fs::path target = (fileName == "Makefile" || extension == ".ld" ? fs::path(newProject.dir) : sourceFolder) / fileName;
        if (fs::exists(target)) continue;
        std::string text = readFile(entry.path().string());
        replaceAll(text, "{{NAME}}", projName);
        std::ofstream(target, std::ios::binary) << text;
      }
      newProject.buildCommand = "make";
    }
    if (!dpcDriver.empty() && !fs::exists(fs::path(newProject.dir) / "DPCplus.arm"))
      fs::copy_file(dpcDriver, fs::path(newProject.dir) / "DPCplus.arm", errorCode);
    writeGfxIncludes(gfxFolder, generatedGfx, cdfj);  // CDFJ: its graphics are the C code's (.gfx.h)
    writeMusicIncludes(musicFolder, generatedMusic, tvStandard);
    copyProjectNotes({exampleDir, templateDir}, newProject.dir, projName, cartridgeText);
    newProject.main = "src/main.asm";
  } else if (projectLanguage == Language::BatariBasic) {
    newProject.main = "src/main.bas";
    // the cartridge: set romsize in the game, and the same format in the project
    // (none chosen: AUTO, the emulators detect it)
    // PXE: batari Basic's ELF kernel, a relocatable ELF the emulators and carts run as ELF
    const bool pxe = format == "PXE";
    newProject.format = format.empty() ? "AUTO" : pxe ? "ELF" : format;
    std::string romSize = "4k";
    for (auto& [formatId, size] : kBasicRomSizes)
      if (newProject.format == formatId) romSize = size;
    const BasicExample* basicExample = nullptr;
    for (auto& entry : kBasicExamples)
      if (!pxe && example == entry.id) basicExample = &entry;
    for (auto& entry : kPxeExamples)
      if (pxe && example == entry.id) basicExample = &entry;
    if (!example.empty() && !basicExample) {
      if (error) *error = F("Template not found in {0}", {example});
      return false;
    }
    fs::path templateDir = fs::path(dataDir()) / "templates" / (basicExample ? basicExample->folder : pxe ? "bb-pxe" : "bb");
    std::string text = readFile((templateDir / "main.bas").string());
    if (text.empty()) {
      if (error) *error = F("Template not found in {0}", {(templateDir / "main.bas").string()});
      return false;
    }
    const bool titleScreen = basicExample && basicExample->titleScreen;
    keepSection(text, "TITLE", titleScreen);
    keepSection(text, "NO_TITLE", !titleScreen);
    replaceAll(text, "{{TITLE_FILE}}", titleScreen ? ", title.vcsgfx" : "");
    replaceAll(text, "{{NAME}}", projName);
    replaceAll(text, "\tset romsize 4k\n", "\tset romsize " + romSize + "\n");
    // batari Basic times the frame for NTSC unless told otherwise (PAL60 keeps NTSC timing)
    // (PXE times its frame itself, for the console it finds itself on)
    if (tvIs50Hz(tvStandard) && !pxe) replaceAll(text, "\tset romsize " + romSize + "\n", "\tset romsize " + romSize + "\n\tset tv pal\n");
    fs::path mainFile = sourceFolder / "main.bas";
    if (!fs::exists(mainFile)) std::ofstream(mainFile, std::ios::binary) << text;
    // its graphics and music (shared.json), the regions of main.bas they fill, and the
    // music player and tables it includes
    std::set<std::string> leaveOut;
    if (!titleScreen) leaveOut.insert("resources/gfx/title.vcsgfx");
    if (!copySharedFiles(templateDir, newProject.dir, {}, error, leaveOut)) return false;
    // (PXE turns NTSC colours into PAL ones itself on a PAL console)
    if (gfx::tvFromProject(tvStandard) == gfx::Tv::PAL && !pxe) graphicsToPal(gfxFolder);
    // the game's pfscorecolor: the top row's colour of the title and of the room
    for (auto [placeholder, fileName] : {std::pair{"{{TITLE_TOP_COLOUR}}", "title.vcsgfx"}, {"{{ROOM_TOP_COLOUR}}", "room.vcsgfx"}}) {
      std::string program = readFile(mainFile.string());
      if (program.find(placeholder) == std::string::npos) continue;
      gfx::Asset asset;
      int colour = 0x0E;
      if (gfx::fromJson(readFile((gfxFolder / fileName).string()), &asset, nullptr) && !asset.playfields.empty())
        colour = asset.playfields[0].pfAt(0);
      replaceAll(program, placeholder, gfx::hexColour(colour));
      std::ofstream(mainFile, std::ios::binary) << program;
    }
    // the game's second enemy: the first one's red ($46) plus $40, a blue, for the TV standard
    if (std::string program = readFile(mainFile.string()); program.find("{{SECOND_ENEMY_COLOUR}}") != std::string::npos) {
      int blue = 0x86;
      if (gfx::tvFromProject(tvStandard) == gfx::Tv::PAL && !pxe) blue = palColourFor(blue);
      replaceAll(program, "{{SECOND_ENEMY_COLOUR}}", gfx::hexColour(blue));
      std::ofstream(mainFile, std::ios::binary) << program;
    }
    fillBasicRegions(mainFile, gfxFolder);
    writeMusicIncludes(musicFolder, generatedMusic, tvStandard);
    copyProjectNotes({templateDir}, newProject.dir, projName,
                     {{"{{FORMAT}}", newProject.format},
                      {"{{ROMSIZE}}", romSize},
                      {"{{TITLE_NOTE}}", titleScreen ? "It starts on a title screen (the cartridge and THE GAME): the button starts a game, and the game ends back there."
                                                     : "There is no title screen: the game starts at once and starts again after the last life."},
                      {"{{TITLE_WHERE}}", titleScreen ? "; on the title, `titleLoop` (the cartridge: `title.vcsgfx`, sprite `cart`)" : ""}});
  } else if (isC(projectLanguage)) {
    bool ace = projectLanguage == Language::AceC;
    if (projName.find_first_of(" \t") != std::string::npos) {
      if (error) *error = T("C projects build with make, which does not take spaces in file names: choose a name without spaces.");
      return false;
    }
    // data/templates/elf (or ace), copied whole, with an example's files over it; {{NAME}}
    // in the Makefile and the sources. ACE: the header's 16-character name and the visible
    // lines (the frame's, less the 63 of the overblank program).
    // ACE's vcsLib example is the ELF project as it is, built with make ace: the same
    // source runs on both kinds of cartridge, and plain make still gives the ELF ROM.
    const bool vcsLibOnAce = ace && example == "vcslib";
    const std::string templateName = ace && !vcsLibOnAce ? "ace" : "elf", exampleName = vcsLibOnAce ? "" : example;
    std::string aceName = (projName + std::string(16, ' ')).substr(0, 16);
    std::string visibleLines = std::to_string((tvIs50Hz(tvStandard) ? 312 : 262) - 63);
    // the vcsLib system type an ELF game gets in its ACE build: ST_NTSC_2600, ST_PAL_2600, ST_PAL60_2600
    std::string aceSystem = tvStandard == "PAL" ? "1" : tvStandard == "PAL60" ? "2" : "0";
    // the TV system VCSIDE The Game's menu starts with, when the cartridge does not say
    std::string menuSystem = tvStandard == "PAL" ? "SYSTEM_PAL" : tvStandard == "PAL60" ? "SYSTEM_PAL60" : "SYSTEM_NTSC";
    fs::path templateDir = fs::path(dataDir()) / "templates" / templateName;
    fs::path exampleDir = fs::path(dataDir()) / "templates" / (templateName + "-" + exampleName);
    if (!fs::exists(templateDir / "Makefile") || (!exampleName.empty() && !fs::exists(exampleDir / "src" / "main.c"))) {
      if (error) *error = F("Template not found in {0}", {(exampleName.empty() ? templateDir : exampleDir).string()});
      return false;
    }
    // the ELF project's other builds, each with its glue folder: the Chameleon image and the
    // ACE file (ACE's vcsLib example is the ACE build alone)
    const bool chameleonBuild = !ace && elfBuilds.chameleon, aceBuild = vcsLibOnAce || (!ace && elfBuilds.ace);
    std::string extraBuilds, glue;
    if (aceBuild)
      extraBuilds += " `make ace`: the same game as `build/" + projName + ".ace` (vcsLib on VCSIDE's ACE bus library in `ace/`)" +
                     (vcsLibOnAce ? ", what this project builds." : ".");
    if (chameleonBuild) extraBuilds += " `make chameleon CHAMELEON_FW=...`: the Chameleon Cart image (`chameleon/`).";
    if (aceBuild && chameleonBuild) glue = ", `ace/` and `chameleon/` (the other cartridges' glue)";
    else if (aceBuild) glue = ", `ace/` (the ACE build's glue)";
    else if (chameleonBuild) glue = ", `chameleon/` (the Chameleon image's glue)";
    const std::vector<std::pair<std::string, std::string>> substitutions = {
        {"{{EXTRA_BUILDS}}", extraBuilds}, {"{{GLUE}}", glue},
        {"{{NAME}}", projName}, {"{{ACE_NAME}}", aceName}, {"{{VISIBLE_LINES}}", visibleLines}, {"{{ACE_SYSTEM}}", aceSystem}, {"{{MENU_SYSTEM}}", menuSystem}};
    std::vector<fs::path> from{templateDir};
    if (!exampleName.empty()) from.insert(from.begin(), exampleDir);  // the first copy of a file wins
    for (auto& sourceDir : from) {
      for (auto& entry : fs::recursive_directory_iterator(sourceDir, errorCode)) {
        if (!entry.is_regular_file()) continue;
        fs::path relativePath = fs::relative(entry.path(), sourceDir), destination = fs::path(newProject.dir) / relativePath;
        if (relativePath == "LICENSE.md" || relativePath == "shared.json" || fs::exists(destination)) continue;
        if (relativePath.begin()->string() == "ace" && !aceBuild) continue;
        if (relativePath.begin()->string() == "chameleon" && !chameleonBuild) continue;
        fs::create_directories(destination.parent_path(), errorCode);
        std::string text = readFile(entry.path().string());
        keepSection(text, "CHAMELEON", chameleonBuild);
        keepSection(text, "ACE", aceBuild);
        for (auto& [search, replacement] : substitutions) replaceAll(text, search, replacement);
        std::ofstream(destination, std::ios::binary) << text;
      }
      // the music, an example's graphics and, in ELF projects, the ACE template's bus
      // library, start code and linker script in ace/, for make ace (next to vcslib_ace.c)
      if (!copySharedFiles(sourceDir, newProject.dir, substitutions, error)) return false;
    }
    if (!aceBuild) fs::remove_all(fs::path(newProject.dir) / "ace", errorCode);  // shared.json's ACE files
    writeGfxIncludes(gfxFolder, generatedGfx, true);
    writeMusicIncludes(musicFolder, generatedMusic, tvStandard);
    newProject.main = "src/main.c";
    newProject.buildCommand = "make";
    newProject.format = ace ? "ACE" : "ELF";
    if (ace) {
      newProject.output = "build/" + projName + ".ace";
      if (vcsLibOnAce) newProject.buildCommand = "make ace";
      newProject.emulator = "gopher2600";                         // the only one that runs ACE
      newProject.emulatorOptions["gopher2600"]["armClock"] = "168";  // the STM32F407's MHz
    }
  }
  // what git leaves out: each person's state of the project (.vcsproj.user) and the build
  if (fs::path ignore = fs::path(newProject.dir) / ".gitignore"; !fs::exists(ignore))
    std::ofstream(ignore, std::ios::binary) << "# Each person's state of the project (open files, layout), and what the build writes\n*.vcsproj.user\n/build/\n";
  if (!newProject.save(error)) return false;
  if (result) *result = newProject;
  return true;
}

bool formatSupported(const std::string& emulator, const std::string& formatId) {
  return !(emulator == "stella" && formatId == "ACE");
}

std::string romFormat(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  char magic[8] = {};
  if (!file.read(magic, sizeof magic)) return "";
  std::string text(magic, sizeof magic);
  for (const char* aceMagic : {"ACE-2600", "ACE-UF00", "ACE-PC00", "ACE-UC00"})
    if (text == aceMagic) return "ACE";
  return "";
}

std::string stellaFormat(const std::string& id) {
  // VCSIDE ids follow Stella's names, except a few aliases.
  if (id == "DPC+") return "DPCP";
  if (id == "CDFJ" || id == "CDFJ+") return "CDF";  // Stella's one type for CDF, CDFJ and CDFJ+
  if (id == "3E+") return "3EP";
  return id;
}

}  // namespace vcs
