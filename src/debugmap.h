// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Source lines <-> CPU addresses and banks, from DASM's listing. Used to send the
// editor's breakpoints to the emulators and to go from an address back to its line.
//
// The listing gives the address in the ROM image (ORG); the CPU sees the
// relocated one (RORG), so RORG/REND are followed per segment. Lines of a macro
// expansion are given to the line that calls the macro.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cycles.h"

namespace vcs {

struct ListedLine {
  std::string file;  // as DASM printed it (resolve it with AddressMap::resolveFiles)
  int line = 0;      // 1-based
  long phys = 0;     // address in the image (ORG)
  long cpu = 0;      // address the 6507 sees (RORG)
  int bytes = 0;
  std::string source;  // the source text of the line (label, mnemonic, operand)
};

struct Breakpoint {
  long cpu = 0;
  int bank = 0;
  const ListedLine* at = nullptr;
};

class AddressMap {
 public:
  // bankSize: from the cartridge format (4096 by default)
  void load(const std::string& listing, const SymbolLookup& lookup, long bankSize);
  // Lines listed in file are given to toFile at lineMap[line - 1] (0: they belong to no
  // line of it and are dropped). For generated sources such as batari Basic's .asm.
  void remap(const std::string& file, const std::vector<int>& lineMap, const std::string& toFile);
  void resolveFiles(const std::function<std::string(const std::string&)>& resolve);
  bool empty() const { return lines_.empty(); }
  int banks() const { return banks_; }
  int bankOf(const ListedLine& listed) const { return bankSize_ > 0 ? (int)((listed.phys - base_) / bankSize_) : 0; }

  // Breakpoint for a source line: the first line with code at or after it, in the
  // same file and before the next line with code of another file position (so a
  // breakpoint on a label or comment goes to the next instruction). isCode
  // decides which listed lines count (e.g. instructions, not .byte).
  std::optional<Breakpoint> breakpointAt(const std::string& file, int line,
                                         const std::function<bool(const ListedLine&)>& isCode) const;
  // Listed lines of a source file, in the order they were assembled.
  std::vector<const ListedLine*> linesIn(const std::string& file) const;
  // Where a listed line is in the ROM file.
  long romOffset(const ListedLine& listed) const { return listed.phys - base_; }
  // Listed lines whose code starts at a CPU address (one per bank where it exists).
  std::vector<const ListedLine*> linesAt(long cpu, int bank = -1) const;

 private:
  std::vector<ListedLine> lines_;
  long base_ = 0, bankSize_ = 4096;
  int banks_ = 1;
};

// Where the code or data of a file enters a new 256-byte page (or bank): a rule goes above
// line (0-based). A page that starts inside a line (a table that crosses it) goes below
// that line, with inside set. Labels just before a new page are kept below the rule.
struct PageBreak {
  int line = 0;
  long address = 0;  // the first address of the page
  int bank = 0;
  bool inside = false;
  bool newBank = false;  // the code before it was in another bank
};
std::vector<PageBreak> pageBreaks(const AddressMap& addresses, const std::string& file, const std::vector<std::string>& lines);

}  // namespace vcs
