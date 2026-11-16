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

enum class Language { Dasm, BatariBasic, ElfC, AceC, TinyElfBasic, Unknown };
// C on a cartridge's ARM (ELF or ACE): built with the project's Makefile and arm-none-eabi-gcc.
inline bool isC(Language language) { return language == Language::ElfC || language == Language::AceC; }
// TinyELF Basic (Al_Nafuur's BASIC machine for the PlusCart and UnoCart): the project's
// program, plain BASIC text, is built into a ROM with TinyELF's sources (data/tinyelf), which
// enters it and runs it at power-on. Built with arm-none-eabi-gcc, run in Gopher2600.
// A cartridge with an ARM that ELF, ACE and TinyELF Basic projects run on: its clock is what
// the emulators pass to the program in MP_CLOCK_HZ and how much the ARM does per 6507 cycle
// (TinyELF Basic sizes its work per frame by it); its memory is what the ELF space report
// measures against. The built-in ones are fixed; the user's own are kept in cartridgeLibrary().
struct ArmCartridge {
  std::string id, name;
  int clockMhz = 216;
  // "pluscart": the UCA firmware's loader (PlusCart and UnoCart), ramKb its buffer;
  // "chameleon": the Chameleon Cart's flash and RAM (flashKb, ramKb), its firmware included.
  std::string memory = "pluscart";
  int flashKb = 0, ramKb = 96;
  bool builtIn = false;
  static constexpr int kUcaClockMhz = 216;       // PlusCart and UnoCart: the UCA firmware runs the STM32F407 at 216 MHz
  static constexpr int kChameleonClockMhz = 64;  // STM32G070
  // The Chameleon Cart's Cortex-M0+ runs ELF games only, not ACE files nor TinyELF.
  bool runsAce() const { return memory != "chameleon"; }
  std::string summary() const;  // "216 MHz, PlusCart loader, 96 KB" for people
  bool sameValues(const ArmCartridge& other) const;
};
const std::vector<ArmCartridge>& builtInArmCartridges();
std::string armCartridgeMemoryName(const std::string& memory);

// The user's own cartridges, in a JSON file of the config folder (cartridges.json); one for
// the whole program, so every window sees the same.
class CartridgeLibrary {
 public:
  void load(const std::string& path);  // no file: none of the user's
  bool save(std::string* error) const;
  const std::vector<ArmCartridge>& mine() const { return mine_; }
  const ArmCartridge* find(const std::string& id) const;  // built-in or the user's
  // Adds it with a new id made from its name (made unique among all names first).
  const ArmCartridge& add(ArmCartridge cartridge);
  bool update(const ArmCartridge& cartridge);  // by id; false for a built-in or unknown one
  bool remove(const std::string& id);
  std::string uniqueName(const std::string& base) const;
  int version() const { return version_; }  // bumped on every change

 private:
  std::vector<ArmCartridge> mine_;
  std::string path_;
  int version_ = 0;
};
CartridgeLibrary& cartridgeLibrary();

enum class IllegalOpcodes { Allow, StableOnly, Warn, Forbid };

// An ELF project's builds besides its ROM, each with its glue folder and its Makefile
// target: make chameleon (chameleon/) and make ace (ace/).
// aceDefault: F7 builds the ACE file (make ace) and F5 runs it, instead of the ELF ROM.
struct ElfBuilds {
  bool chameleon = true, ace = true, aceDefault = false;
};

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
  // The project's folders, relative to it (new projects: src, resources, generated and
  // build; empty in older ones, which keep everything where it is):
  // - sourceDir: the programs (DASM looks for includes there; make gets SRC_DIR);
  // - resourcesDir: the graphics and music, in its gfx/ and music/ folders;
  // - generatedDir: what the graphics and music editors export on save (.gfx.asm, .gfx.h,
  //   music.asm, music.h), in its gfx/ and music/ folders; empty: next to each file;
  // - buildDir: what the build writes (make gets BUILD_DIR; batari Basic's files are moved
  //   there); empty: the ROM's folder.
  std::string sourceDir, resourcesDir, generatedDir, buildDir;
  // optional: a command of the project's own (e.g. "make" or "sh build.sh") instead of the
  // tool; with &&, |, ; or > it runs through the shell
  std::string buildCommand;
  // The tools this project builds with, when not the ones in Settings > Tools: tool id
  // ("dasm", "bb") -> path, relative to the project or absolute.
  std::map<std::string, std::string> toolPaths;
  std::string buildFlags;       // more arguments for DASM (e.g. "-DPAL=1 -T1")
  int spaceWarnPercent = 90;    // warn when a bank or RAM area is fuller than this
  int stackReserve = 8;         // bytes of 2600 RAM to keep free for the stack
  // ELF, ACE and TinyELF Basic: the cartridge the emulators imitate (its ARM clock) and the
  // space report measures against, by id: a built-in one ("unocart", "pluscart", "chameleon")
  // or one of the user's (cartridgeLibrary()); empty: the language's own (see
  // armCartridgeInfo()). A cartridge that is not built in is also kept in the project
  // (cartridgeCopy, "cartridge_definition"), so it opens the same where the library lacks it.
  std::string cartridge;
  ArmCartridge cartridgeCopy;
  // True for the projects whose ROM runs on a cartridge's ARM: ELF, ACE and TinyELF Basic.
  bool runsOnArmCartridge() const;
  // The cartridge in effect: `cartridge` from the library (or its copy) when this language can
  // run on it, else the language's default (ELF: the Chameleon Cart; ACE and TinyELF Basic:
  // the PlusCart). An empty id when runsOnArmCartridge() is false.
  ArmCartridge armCartridgeInfo() const;
  // What this project can choose, in menu order: the built-in ones, the user's, and its own
  // copy when the library lacks it.
  std::vector<ArmCartridge> armCartridgeChoices() const;
  bool canUse(const ArmCartridge& candidate) const;  // the Chameleon Cart's memory: ELF only
  void useCartridge(const ArmCartridge& chosen);
  std::string armCartridge() const { return armCartridgeInfo().id; }
  int armClockMhz() const { return armCartridgeInfo().clockMhz; }
  std::string memoryCartridge() const { return armCartridgeInfo().memory; }  // how the space report measures it
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
  // The folder where the export of a .vcsgfx ("gfx") or .vcsmusic ("music") goes:
  // <generatedDir>/<kind>, plus the subfolder of <resourcesDir>/<kind> the file is in; next to
  // the file when the project has no generatedDir or the file is not in the project.
  std::string generatedFolder(const std::string& sourcePath, const std::string& kind) const;
  std::string outputPath() const { return abs(output); }
  // An ELF game with a 7800 side, which Test7800 runs: its ROM (or, before it is built, its C
  // sources) has the .elf7800 section, which tells the firmware to start it in MARIA mode.
  bool hasA7800Side() const;
  // Whether an emulator runs this project: its format, and Test7800 a 7800 side.
  bool emulatorRuns(const std::string& emulatorId) const;
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
  // next to the source (basicWritten); in a project with a buildDir they are moved there
  // after it runs, with the files it leaves in the folder it runs in (collectBasicOutputs).
  // basicOutput is where they are then.
  std::string basicWritten(const std::string& suffix) const { return abs(main) + suffix; }
  std::string basicOutput(const std::string& suffix) const;
  // The build folder: buildDir, or the ROM's folder.
  std::string buildFolder() const;
  // Folders DASM looks in besides the project's own and include_dirs: the source folder and
  // the generated gfx/ and music/ folders, when the project has them.
  std::vector<std::string> layoutIncludeDirs() const;
  void collectBasicOutputs() const;
  // batari Basic's PXE kernel (format ELF) with a pxe.c in the project's folder: that C is
  // compiled and linked into the ELF batari Basic wrote, at <main>.bas.linked; it wraps
  // vcsEndOverblank, which the kernel calls once per frame (see the PXE template).
  // Empty when the project has none.
  std::string pxeSource() const;
  // batari Basic's PXE kernel (format ELF): built with the PXE beta (tool "bb-pxe"), unless
  // the project names a batari Basic of its own
  bool pxe() const { return language == Language::BatariBasic && format == "ELF" && buildCommand.empty(); }
  std::string basicTool() const { return pxe() && !toolPaths.count("bb") ? "bb-pxe" : "bb"; }
  std::vector<std::string> pxeLinkArgs(Toolchain& toolchain, std::string* error) const;
  // TinyELF Basic: the compiler, the one VCSIDE uses unless it lacks the C library TinyELF
  // needs (newlib: Homebrew's arm-none-eabi-gcc has none) and another one found has it.
  std::string tinyElfCompiler(Toolchain& toolchain) const;
  std::vector<std::string> runArgs(Toolchain& toolchain, bool debug, std::string* error) const;

  // The tool this project uses: its own path if it has one and it exists, else the one
  // VCSIDE found; "" if neither.
  std::string toolPath(Toolchain& toolchain, const std::string& id) const;

  // The project file a path names: the path itself if it is a .vcsproj, or the .vcsproj in a
  // folder (the first, by name, if there are several). Empty if neither.
  static std::string projectFileAt(const std::string& path);

  // A folder without a .vcsproj that can be built: one that builds by itself (Makefile,
  // build.sh, build.bat, make.bat), with that command, the main source and the ROM it seems
  // to write; or one with a DASM source (a "processor" line) or a batari Basic one, in it or
  // in src/, built by VCSIDE. False if none (C needs a Makefile).
  static bool detect(const std::string& directory, Project* result);

  // Creates a new project folder with a template for the language.
  // example: "" for the empty template, or one of examples(language, format).
  // format: the cartridge, one of cartridgeFormats(language) ("": 4K, or the example's own).
  // elfBuilds: an ELF project's builds besides its ROM (ElfBuilds).
  static bool create(const std::string& directory, const std::string& projectName, Language projectLanguage, const std::string& tvStandard,
                     Project* result, std::string* error, const std::string& example = "", const std::string& format = "",
                     const ElfBuilds& elfBuilds = {});
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
// The emulators VCSIDE runs, in menu order: Gopher2600 and Stella (2600), Test7800 (the
// 7800 side of a Chameleon Cart ELF game).
const std::vector<std::string>& emulatorIds();
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
