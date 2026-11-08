// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// A VCSIDE project: a folder with a .vcsproj (JSON) describing how to build and run it.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "child_process.h"
#include "toolchain.h"

namespace vcs {

enum class Language { Dasm, BatariBasic, ElfC, Unknown };
enum class IllegalOpcodes { Allow, StableOnly, Warn, Forbid };

struct Project {
  std::string file;             // absolute path of the .vcsproj
  std::string dir;              // folder that holds it
  std::string name = "game";
  Language language = Language::Dasm;
  std::string main = "main.asm";  // relative to dir
  std::string output = "build/game.bin";
  std::string format = "4K";      // cartridge format id (formats.json)
  std::string tv = "NTSC";        // NTSC, PAL, PAL60, SECAM, NTSC50, SECAM60, PAL-M, AUTO
  std::string emulator = "stella";
  IllegalOpcodes illegal = IllegalOpcodes::Warn;
  std::vector<std::string> includeDirs;
  // optional: a command of the project's own (e.g. "make" or "sh build.sh") instead of the
  // tool; with &&, |, ; or > it runs through the shell
  std::string buildCommand;
  // The tools this project builds with, when not the ones in Settings > Tools: tool id
  // ("dasm", "bb") -> path, relative to the project or absolute.
  std::map<std::string, std::string> toolPaths;
  std::string buildFlags;       // more arguments for DASM (e.g. "-DPAL=1 -T1")
  int spaceWarnPercent = 90;    // warn when a bank or RAM area is fuller than this
  int stackReserve = 8;         // bytes of 2600 RAM to keep free for the stack
  // Launch options per emulator: option id (emulators.json) -> value. Unset = not passed.
  std::map<std::string, std::map<std::string, std::string>> emulatorOptions;
  std::map<std::string, std::string> emulatorArgs;  // extra arguments per emulator, before the ROM

  bool load(const std::string& path, std::string* error);
  bool save(std::string* error) const;

  std::string abs(const std::string& rel) const;
  std::string outputPath() const { return abs(output); }
  std::string stem() const;  // output path without extension (for .lst/.sym)

  // DASM run that writes the emitted blocks (-f2) to <stem>.ras, for the space report.
  std::vector<std::string> rasArgs(Toolchain& tc) const;
  // Command lines; empty vector + error when a tool is missing. workDir and env, when
  // given, receive where to run it and the variables it needs (project folder, none).
  // Toolchain ids the build needs (dasm; bb and, for 1.9, wasmtime; arm-gcc and make).
  std::vector<std::string> buildTools(Toolchain& tc) const;
  std::vector<std::string> buildArgs(Toolchain& tc, std::string* error, std::string* workDir = nullptr,
                                     EnvVars* env = nullptr) const;
  // batari Basic writes <main>.bin, .asm, .list.txt (.lst before 1.9) and .symbol.txt (.sym)
  // next to the source; these are the paths.
  std::string basicOutput(const std::string& suffix) const { return abs(main) + suffix; }
  std::vector<std::string> runArgs(Toolchain& tc, bool debug, std::string* error) const;

  // The tool this project uses: its own path if it has one and it exists, else the one
  // VCSIDE found; "" if neither.
  std::string toolPath(Toolchain& tc, const std::string& id) const;

  // A folder that builds by itself (Makefile, build.sh, build.bat, make.bat): a project for
  // it, with that command, the main source and the ROM it seems to write. False if none.
  static bool detect(const std::string& dir, Project* out);

  // Creates a new project folder with a template for the language.
  // example: "" for the empty template, or one of exampleIds(lang).
  static bool create(const std::string& dir, const std::string& name, Language lang, const std::string& tv,
                     Project* out, std::string* error, const std::string& example = "");
  // Examples a new project can start from: id and name (for people, to translate).
  static std::vector<std::pair<std::string, const char*>> examples(Language lang);
};

// TV standards a project can choose, and whether an emulator has each one.
const std::vector<std::string>& tvStandardIds();
bool tvSupported(const std::string& emulator, const std::string& tv);
bool tvIs50Hz(const std::string& tv);
std::vector<std::string> splitCommand(const std::string& s);  // "a \"b c\"" -> a, b c
std::string stellaFormat(const std::string& formatId);  // value for Stella's -bs
const char* languageId(Language l);
Language languageFromId(const std::string& s);
const char* illegalId(IllegalOpcodes i);
IllegalOpcodes illegalFromId(const std::string& s);

}  // namespace vcs
