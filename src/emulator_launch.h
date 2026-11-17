// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Launching the emulators with the project's options (data/kb/emulators.json).
//
// Neither emulator can take these options without keeping them: Stella saves
// every command-line option into its settings when it quits, and Gopher2600
// only reads most of them from its preferences file. So VCSIDE gives each one a
// private settings folder under its own config folder, copied from the user's
// settings before every launch: what the user set in the emulator still applies,
// and nothing VCSIDE passes ends up in the user's own settings.
//
//   Stella, Unix and Windows builds: -basedir <profile>
//   Stella.app on macOS (ignores -basedir): HOME=<profile>, so its
//     ~/Library/Application Support/Stella is the profile's
//   Gopher2600: portable mode (portable.txt in the working folder, the
//     preferences in ./Gopher2600_UserData), only when a preference is set
//   Test7800: portable mode too (./Test7800_UserData), always: it has only flags,
//     but it writes its SaveKey and save files there
#pragma once
#include <map>
#include <string>
#include <vector>

#include "kb.h"
#include "child_process.h"
#include "project.h"

namespace vcs {

struct EmulatorLaunch {
  std::vector<std::string> args;  // from Project::runArgs: program first, ROM last
  std::string workDir;
  EnvVars env;
  std::vector<std::string> notes;  // for the output panel
};

// Stella's -dev.arm.mips range: its ELF ARM speed in MIPS (CartELF.hxx), passed to the
// program as its clock.
constexpr int kStellaArmMipsMin = 50, kStellaArmMipsMax = 300;

// Gopher2600 stops an ARM run after 1,500,000 ARM cycles without a sync with the
// 6507 (cycleLimit in hardware/memory/cartridge/arm/arm.go), whatever the clock.
// TinyELF Basic sizes the work it does between two frames from the clock (its
// budget is clock / 650000 statements), so above about 100 MHz a program with
// long statements passes that limit and Gopher2600 stops showing frames (measured:
// fine at 70 and 100 MHz, no frames at 140, 168 and 216). VCSIDE runs TinyELF
// Basic in Gopher2600 at its default clock at most.
constexpr int kGopherTinyElfMaxClockMhz = 70;

// The ARM clock Gopher2600 gets for the project: the emulated cartridge's, but at
// most kGopherTinyElfMaxClockMhz for TinyELF Basic.
int gopherArmClockMhz(const Project& project);

// The arguments the project's options add for an emulator (shown in the options
// dialog) and the Gopher2600 preferences they set. For an ELF, ACE or TinyELF Basic project
// the emulated cartridge's clock replaces the ARM clock options (armClock, dev.arm.mips).
std::vector<std::string> emulatorOptionArgs(const Project& project, const EmulatorInfo& emulator,
                                            std::map<std::string, std::string>* preferences = nullptr);

// Inserts the options and the extra arguments before the ROM and prepares the
// private settings folder under profileRoot (configDir()/emulators). The user's
// own settings are looked for where each emulator keeps them; userHome replaces
// the home folder (HOME, or %APPDATA% on Windows) for tests.
bool prepareEmulatorLaunch(const Project& project, const KnowledgeBase& knowledgeBase, const std::string& profileRoot,
                           EmulatorLaunch& launch, std::string* error, const std::string& userHome = {});

// "key :: value" lines of a Gopher2600 preferences file with some keys replaced
// (or added); the header line is kept.
std::string mergeGopherPreferences(const std::string& text, const std::map<std::string, std::string>& preferences);

}  // namespace vcs
