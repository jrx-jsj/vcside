// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Space used in ROM (per bank), in the 2600's 128 bytes of RAM and in the
// cartridge's extra RAM, from DASM's output.
//
// ROM: a second DASM run with -f2 writes every block of emitted bytes with its
// origin and length, so the exact bytes used are known (ALIGN padding and ds
// fill count as used). RAM: the uninitialised (SEG.U) lines of the listing,
// with the size of each ds read from the source line.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "builder_parse.h"
#include "cycles.h"
#include "format_space.h"

namespace vcs {

// Blocks of emitted bytes from a DASM -f2 file (origin, length, data...).
std::vector<Range> parseDasmRas(const std::string& data);

// A declaration in an uninitialised segment (SEG.U ... ds n).
struct RamDecl {
  std::string file;  // as printed in the listing
  int line = 0;
  long addr = 0, size = 0;
  std::string label;
};
// Reads the RAM declarations of a DASM listing. readSource(file) returns the
// lines of a source file (to read each ds); lookup resolves symbols in ds sizes.
std::vector<RamDecl> parseDasmListingRam(const std::string& listing,
                                         const std::function<std::vector<std::string>(const std::string&)>& readSource,
                                         const SymbolLookup& lookup);
// Initialised lines of the listing (address -> file:line), to point diagnostics at code.
struct CodeLine {
  std::string file;
  int line = 0;
  long addr = 0;
  bool fill = false;  // ds / align padding (may cover hotspots and RAM ports)
};
std::vector<CodeLine> parseDasmListingCode(const std::string& listing);

struct SpaceOptions {
  int warnPercent = 90;   // warn when a bank or a RAM area is fuller than this
  int stackReserve = 8;   // bytes of 2600 RAM kept free above the variables for the stack
  long basicRomSize = 0;  // batari Basic: the ROM's size when it is not banks of 4K (PXE: 60K)
};

struct SpaceReport {
  struct Area {
    std::string name;
    long used = 0, size = 0;
    bool measured = true;
    std::string note;
  };
  std::vector<Area> banks;   // ROM, one per bank
  long romFileSize = 0;
  Area ram;                  // 2600 RAM ($80-$FF)
  long stackRoom = 0;        // bytes from the highest variable to $FF
  std::optional<Area> cartRam;
  std::vector<Diagnostic> diagnostics;
  std::string summary() const;  // short text for the status bar
};

SpaceReport analyzeSpace(const std::vector<Range>& rom, long romFileSize, const std::vector<RamDecl>& ram,
                         const std::vector<CodeLine>& code, const FormatSpace& formatSpace, const SpaceOptions& options);

// batari Basic prints "N bytes of ROM space left" (one bank) or "... left in bank K"
// (K from 1, 4 KB banks); banks here count from 0, as in the emulators. Its RAM is
// not measured: the variables are fixed.
SpaceReport parseBasicSpace(const std::string& output, long romFileSize, const SpaceOptions& options);

}  // namespace vcs
