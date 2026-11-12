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

enum class Language { Dasm, BatariBasic, ElfC, AceC, Unknown };
// C on a cartridge's ARM (ELF or ACE): built with the project's Makefile and arm-none-eabi-gcc.
inline bool isC(Language language) { return language == Language::ElfC || language == Language::AceC; }
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
  std::string emulator = "gopher2600";
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
  // ELF: the cartridge the space report measures against, "chameleon" or "pluscart"
  // (PlusCart and UnoCart), and its flash and RAM in KB (0: the cartridge's own sizes).
  std::string elfCartridge = "chameleon";
  int elfFlashKb = 0, elfRamKb = 0;
  // Launch options per emulator: option id (emulators.json) -> value. Unset = not passed.
  std::map<std::string, std::map<std::string, std::string>> emulatorOptions;
  std::map<std::string, std::string> emulatorArgs;  // extra arguments per emulator, before the ROM

  std::map<std::string, std::string> otherKeys;  // keys VCSIDE does not use, as JSON text: kept on save

  bool load(const std::string& path, std::string* error);
  bool save(std::string* error) const;
  // The .vcsproj's text, and reading it back (path: where it lives, for dir and file).
  std::string toJson() const;
  bool fromJson(const std::string& text, const std::string& path, std::string* error);

  std::string abs(const std::string& relativePath) const;
  std::string outputPath() const { return abs(output); }
  std::string stem() const;  // output path without extension (for .lst/.sym)

  // DASM run that writes the emitted blocks (-f2) to <stem>.ras, for the space report.
  std::vector<std::string> rasArgs(Toolchain& toolchain) const;
  // Command lines; empty vector + error when a tool is missing. workDir and env, when
  // given, receive where to run it and the variables it needs (project folder, none).
  // Toolchain ids the build needs (dasm; bb and, for 1.9, wasmtime; arm-gcc and make).
  std::vector<std::string> buildTools(Toolchain& toolchain) const;
  std::vector<std::string> buildArgs(Toolchain& toolchain, std::string* error, std::string* workDir = nullptr,
                                     EnvVars* env = nullptr) const;
  // batari Basic writes <main>.bin, .asm, .list.txt (.lst before 1.9) and .symbol.txt (.sym)
  // next to the source; these are the paths.
  std::string basicOutput(const std::string& suffix) const { return abs(main) + suffix; }
  std::vector<std::string> runArgs(Toolchain& toolchain, bool debug, std::string* error) const;

  // The tool this project uses: its own path if it has one and it exists, else the one
  // VCSIDE found; "" if neither.
  std::string toolPath(Toolchain& toolchain, const std::string& id) const;

  // A folder without a .vcsproj that can be built: one that builds by itself (Makefile,
  // build.sh, build.bat, make.bat), with that command, the main source and the ROM it seems
  // to write; or one with a DASM source (a "processor" line) or a batari Basic one, in it or
  // in src/, built by VCSIDE. False if none (C needs a Makefile).
  static bool detect(const std::string& directory, Project* result);

  // Creates a new project folder with a template for the language.
  // example: "" for the empty template, or one of examples(language, format).
  // format: the cartridge, one of cartridgeFormats(language) ("": 4K, or the example's own).
  static bool create(const std::string& directory, const std::string& projectName, Language projectLanguage, const std::string& tvStandard,
                     Project* result, std::string* error, const std::string& example = "", const std::string& format = "");
  // Examples a new project can start from: id and name (for people, to translate). With a
  // format, only the ones made for that cartridge.
  static std::vector<std::pair<std::string, const char*>> examples(Language language, const std::string& format = "");
  // The cartridges a new DASM or batari Basic project can start on, as formats.json ids
  // (the ones with a template that builds), and whether there is an empty template for one
  // (DPC+ and CDFJ only come as their examples). Empty for C: ELF and ACE choose otherwise.
  static std::vector<std::string> cartridgeFormats(Language language);
  static bool hasEmptyTemplate(Language language, const std::string& format);
  // DPC+'s ARM driver from the batari Basic VCSIDE installs (or $bB's); empty if there is none.
  static std::string dpcPlusDriver();
  // CDFJ's ARM driver (cdfj_driver.bin) is not included with VCSIDE: its licence does not let
  // VCSIDE pass it on, so the user adds their own copy. checkCdfjDriver says whether a file
  // is one (2048 bytes, the driver's "CDFJ" signature and its four entry points);
  // addCdfjDriver checks it and copies it into the project folder as cdfj_driver.bin.
  static constexpr const char* kCdfjDriverFile = "cdfj_driver.bin";
  static bool checkCdfjDriver(const std::string& path, std::string* error);
  static bool addCdfjDriver(const std::string& path, const std::string& projectDir, std::string* error);
};

// TV standards a project can choose, and whether an emulator has each one.
const std::vector<std::string>& tvStandardIds();
// The ones a language can build for (AUTO not included): batari Basic has `set tv ntsc|pal`
// (PAL60 is its NTSC timing with PAL colours) and vcsLib detects NTSC, PAL and PAL60;
// a DASM kernel is the programmer's, so any.
std::vector<std::string> tvStandardsFor(Language language);
std::string tvLabel(const std::string& tv);  // for people: AUTO is "Multiple (in the code)"
// AUTO in a project: it runs on several systems, handled in its code (a DASM template
// chooses with -DPAL=1, vcsLib detects it); the emulator is not told one and detects it.
bool tvSupported(const std::string& emulator, const std::string& tv);
bool tvIs50Hz(const std::string& tv);
std::vector<std::string> splitCommand(const std::string& command);  // "a \"b c\"" -> a, b c
std::string stellaFormat(const std::string& formatId);  // value for Stella's -bs
// Whether an emulator runs a cartridge format: Stella has no ACE (Gopher2600 runs them all).
bool formatSupported(const std::string& emulator, const std::string& formatId);
// The format a ROM file shows by itself, from its first bytes: "ACE" for an ACE header
// (ACE-2600, ACE-UF00, ACE-PC00, ACE-UC00), else "" (the emulator detects it).
std::string romFormat(const std::string& path);
const char* languageId(Language language);
Language languageFromId(const std::string& id);
const char* illegalId(IllegalOpcodes policy);
IllegalOpcodes illegalFromId(const std::string& id);

}  // namespace vcs
