// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Space used by an ELF cartridge game, read straight from the ELF files.
//
// The ROM is a relocatable object (sections, no addresses): its sections give the
// code, the code that runs in RAM (.RamFunc), the constants and the variables. What
// limits it depends on the cartridge the project is for:
// - Chameleon Cart (STM32G070RB: 128 KB of flash and 36 KB of RAM, its firmware
//   included). When the project also has the Chameleon image (<rom>-chameleon.elf,
//   linked with the firmware), its program headers give the exact flash and RAM;
//   otherwise the firmware's share is estimated.
// - PlusCart and UnoCart (UCA firmware 2.3.32, STM32F407): a file up to 96 KB is read
//   whole into a RAM buffer; a bigger one is written to flash and only the code, the
//   variables with a value and the constants with relocations (pointers) are copied to
//   the buffer. The zeroed variables go to the 64 KB of CCM RAM.
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
bool readElf(const std::string& data, ElfFile& elf, std::string* error);

enum class ElfCartridge { Chameleon, PlusCart };  // PlusCart: also the UnoCart (same firmware)
// What the space is measured against; flash and ram in bytes (flash unused on the PlusCart).
struct ElfLimits {
  ElfCartridge cartridge = ElfCartridge::Chameleon;
  long flash = 0, ram = 0;
};
// id: "chameleon" or "pluscart"; flashKb and ramKb: 0 for the cartridge's own sizes.
ElfLimits elfLimits(const std::string& cartridgeId, int flashKb, int ramKb);
long defaultElfFlashKb(const std::string& cartridgeId);  // 0 when it has none to set
long defaultElfRamKb(const std::string& cartridgeId);

struct ElfSpace {
  struct Area {
    std::string name;
    long used = 0, size = 0;
  };
  long fileSize = 0;
  long code = 0, ramCode = 0, constants = 0, data = 0, bss = 0;  // the game, from the ROM
  long relocatedConstants = 0;  // the constants with relocations (part of constants)
  ElfCartridge cartridge = ElfCartridge::Chameleon;
  Area flash, ram;  // flash: Chameleon Cart only (size 0 on the PlusCart)
  Area ccm;         // PlusCart and UnoCart: the zeroed variables (size 0 on the Chameleon)
  bool exact = false;  // Chameleon: from the Chameleon image (else estimated)
  bool wholeFile = false;  // PlusCart: the file fits the buffer and is read whole into it
  std::vector<Diagnostic> diagnostics;
  std::string summary() const;
};

// chameleonImage: the image linked with the firmware, or nullptr (Chameleon only).
// warnPercent as in SpaceOptions.
ElfSpace analyzeElfSpace(const ElfFile& rom, long fileSize, const ElfFile* chameleonImage, const ElfLimits& limits, int warnPercent);

// Chameleon Cart sizes (STM32G070RB) and what its firmware takes, measured on the
// ELF template of 2025-10-23 (flash: vectors, HAL, vcsLib, logo; RAM: its variables,
// 1 KB of stack).
constexpr long kChameleonFlash = 128 * 1024, kChameleonRam = 36 * 1024;
constexpr long kChameleonFirmwareFlash = 10100, kChameleonFirmwareRam = 6000;
// PlusCart and UnoCart (UCA firmware, main.h: BUFFER_SIZE and CCM_RAM_SIZE).
constexpr long kPlusCartBuffer = 96 * 1024, kPlusCartCcm = 64 * 1024;

}  // namespace vcs
