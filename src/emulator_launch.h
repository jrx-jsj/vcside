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

// The arguments the project's options add for an emulator (shown in the options
// dialog) and the Gopher2600 preferences they set.
std::vector<std::string> emulatorOptionArgs(const Project& p, const EmulatorInfo& emu,
                                            std::map<std::string, std::string>* prefs = nullptr);

// Inserts the options and the extra arguments before the ROM and prepares the
// private settings folder under profileRoot (configDir()/emulators). The user's
// own settings are looked for where each emulator keeps them; userHome replaces
// the home folder (HOME, or %APPDATA% on Windows) for tests.
bool prepareEmulatorLaunch(const Project& p, const KnowledgeBase& kb, const std::string& profileRoot,
                           EmulatorLaunch& launch, std::string* error, const std::string& userHome = {});

// "key :: value" lines of a Gopher2600 preferences file with some keys replaced
// (or added); the header line is kept.
std::string mergeGopherPreferences(const std::string& text, const std::map<std::string, std::string>& prefs);

}  // namespace vcs
