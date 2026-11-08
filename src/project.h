// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// A VCSIDE project: a folder with a .vcsproj (JSON) describing how to build and run it.
#pragma once
#include <string>
#include <vector>

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
  std::string tv = "NTSC";        // NTSC, PAL, PAL60, SECAM
  std::string emulator = "stella";
  IllegalOpcodes illegal = IllegalOpcodes::Warn;
  std::vector<std::string> includeDirs;
  std::string buildCommand;     // optional: custom command (e.g. "make"), overrides the tool

  bool load(const std::string& path, std::string* error);
  bool save(std::string* error) const;

  std::string abs(const std::string& rel) const;
  std::string outputPath() const { return abs(output); }
  std::string stem() const;  // output path without extension (for .lst/.sym)

  // Command lines; empty vector + error when a tool is missing.
  std::vector<std::string> buildArgs(Toolchain& tc, std::string* error) const;
  std::vector<std::string> runArgs(Toolchain& tc, bool debug, std::string* error) const;

  // Creates a new project folder with a template for the language.
  static bool create(const std::string& dir, const std::string& name, Language lang, const std::string& tv,
                     Project* out, std::string* error);
};

std::string stellaFormat(const std::string& formatId);  // value for Stella's -bs
const char* languageId(Language l);
Language languageFromId(const std::string& s);
const char* illegalId(IllegalOpcodes i);
IllegalOpcodes illegalFromId(const std::string& s);

}  // namespace vcs
