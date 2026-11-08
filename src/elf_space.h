// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Space used by an ELF cartridge game, read straight from the ELF files.
//
// The ROM is a relocatable object (sections, no addresses): its sections give the
// code, the code that runs in RAM (.RamFunc), the constants and the variables. The
// cartridge that limits it is the Chameleon Cart (STM32G070RB: 128 KB of flash and
// 36 KB of RAM, its firmware included). When the project also has the Chameleon
// image (<rom>-chameleon.elf, linked with the firmware), its program headers give
// the exact flash and RAM; otherwise the firmware's share is estimated.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "builder_parse.h"

namespace vcs {

struct ElfFile {
  struct Section {
    std::string name;
    uint32_t type = 0, flags = 0, addr = 0, size = 0;
  };
  struct Segment {
    uint32_t type = 0, vaddr = 0, paddr = 0, filesz = 0, memsz = 0;
  };
  int type = 0;  // 1 = relocatable (ROM), 2 = executable (Chameleon image)
  int machine = 0;
  std::vector<Section> sections;
  std::vector<Segment> segments;
};
// 32-bit little-endian ELF; false and an error if it is not one.
bool readElf(const std::string& data, ElfFile& out, std::string* error);

struct ElfSpace {
  struct Area {
    std::string name;
    long used = 0, size = 0;
  };
  long fileSize = 0;
  long code = 0, ramCode = 0, constants = 0, data = 0, bss = 0;  // the game, from the ROM
  Area flash, ram;  // Chameleon Cart
  bool exact = false;  // from the Chameleon image (else estimated)
  std::vector<Diagnostic> diagnostics;
  std::string summary() const;
};

// chameleon: the image linked with the firmware, or nullptr. warnPercent as in SpaceOptions.
ElfSpace analyzeElfSpace(const ElfFile& rom, long fileSize, const ElfFile* chameleon, int warnPercent);

// Chameleon Cart sizes (STM32G070RB) and what its firmware takes, measured on the
// ELF template of 2025-10-23 (flash: vectors, HAL, vcsLib, logo; RAM: its variables,
// 1 KB of stack).
constexpr long kChameleonFlash = 128 * 1024, kChameleonRam = 36 * 1024;
constexpr long kChameleonFirmwareFlash = 10100, kChameleonFirmwareRam = 6000;

}  // namespace vcs
