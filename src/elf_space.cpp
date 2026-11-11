// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "elf_space.h"

#include <algorithm>

#include "i18n.h"

namespace vcs {

static uint32_t u32(const std::string& data, size_t offset) {
  if (offset + 4 > data.size()) return 0;
  return (uint32_t)(uint8_t)data[offset] | (uint32_t)(uint8_t)data[offset + 1] << 8 | (uint32_t)(uint8_t)data[offset + 2] << 16 |
         (uint32_t)(uint8_t)data[offset + 3] << 24;
}
static uint32_t u16(const std::string& data, size_t offset) {
  if (offset + 2 > data.size()) return 0;
  return (uint32_t)(uint8_t)data[offset] | (uint32_t)(uint8_t)data[offset + 1] << 8;
}

bool readElf(const std::string& data, ElfFile& elf, std::string* error) {
  elf = ElfFile();
  if (data.size() < 52 || data.compare(0, 4, "\x7f" "ELF") != 0 || data[4] != 1 || data[5] != 1) {
    if (error) *error = T("not a 32-bit little-endian ELF file");
    return false;
  }
  elf.type = (int)u16(data, 16);
  elf.machine = (int)u16(data, 18);
  uint32_t phoff = u32(data, 28), shoff = u32(data, 32);
  uint32_t phentsize = u16(data, 42), phnum = u16(data, 44), shentsize = u16(data, 46), shnum = u16(data, 48), shstrndx = u16(data, 50);
  for (uint32_t i = 0; i < phnum && phentsize >= 32; i++) {
    size_t headerOffset = phoff + (size_t)i * phentsize;
    if (headerOffset + 32 > data.size()) break;
    elf.segments.push_back({u32(data, headerOffset), u32(data, headerOffset + 8), u32(data, headerOffset + 12), u32(data, headerOffset + 16), u32(data, headerOffset + 20)});
  }
  std::vector<uint32_t> nameOff;
  for (uint32_t i = 0; i < shnum && shentsize >= 40; i++) {
    size_t headerOffset = shoff + (size_t)i * shentsize;
    if (headerOffset + 40 > data.size()) break;
    ElfFile::Section section;
    section.type = u32(data, headerOffset + 4);
    section.flags = u32(data, headerOffset + 8);
    section.addr = u32(data, headerOffset + 12);
    section.size = u32(data, headerOffset + 20);
    nameOff.push_back(u32(data, headerOffset));
    elf.sections.push_back(section);
  }
  if (shstrndx < elf.sections.size()) {
    size_t strOff = u32(data, shoff + (size_t)shstrndx * shentsize + 16);
    for (size_t i = 0; i < elf.sections.size(); i++) {
      size_t offset = strOff + nameOff[i];
      if (offset < data.size()) elf.sections[i].name = data.c_str() + offset;
    }
  }
  return true;
}

static int percent(long used, long size) { return size > 0 ? (int)(used * 100 / size) : 0; }

long defaultElfFlashKb(const std::string& cartridgeId) { return cartridgeId == "pluscart" ? 0 : kChameleonFlash / 1024; }
long defaultElfRamKb(const std::string& cartridgeId) { return (cartridgeId == "pluscart" ? kPlusCartBuffer : kChameleonRam) / 1024; }

ElfLimits elfLimits(const std::string& cartridgeId, int flashKb, int ramKb) {
  ElfLimits limits;
  limits.cartridge = cartridgeId == "pluscart" ? ElfCartridge::PlusCart : ElfCartridge::Chameleon;
  limits.flash = (flashKb > 0 ? flashKb : defaultElfFlashKb(cartridgeId)) * 1024L;
  limits.ram = (ramKb > 0 ? ramKb : defaultElfRamKb(cartridgeId)) * 1024L;
  return limits;
}

static long wordAligned(long size) { return (size + 3) & ~3L; }

ElfSpace analyzeElfSpace(const ElfFile& rom, long fileSize, const ElfFile* chameleonImage, const ElfLimits& limits, int warnPercent) {
  enum : uint32_t { WRITE = 1, ALLOC = 2, EXEC = 4, NOBITS = 8, REL = 9, LOAD = 1 };
  ElfSpace elfSpace;
  elfSpace.fileSize = fileSize;
  elfSpace.cartridge = limits.cartridge;
  auto hasRelocations = [&rom](const std::string& name) {
    for (auto& section : rom.sections)
      if (section.type == REL && section.name == ".rel" + name) return true;
    return false;
  };
  long bufferUsed = 0, ccmUsed = 0;  // PlusCart: as its loader places them, word aligned
  for (auto& section : rom.sections) {
    if (!(section.flags & ALLOC)) continue;
    bool constant = false;
    if (section.name.rfind(".RamFunc", 0) == 0) elfSpace.ramCode += section.size;
    else if (section.flags & EXEC) elfSpace.code += section.size;
    else if (section.type == NOBITS) elfSpace.bss += section.size;
    else if (section.flags & WRITE) elfSpace.data += section.size;
    else elfSpace.constants += section.size, constant = true;
    bool relocated = constant && hasRelocations(section.name);
    if (relocated) elfSpace.relocatedConstants += section.size;
    if (section.type == NOBITS) ccmUsed += wordAligned(section.size);
    else if (!constant || relocated) bufferUsed += wordAligned(section.size);
  }
  if (limits.cartridge == ElfCartridge::PlusCart) {
    elfSpace.ram = {T("RAM"), 0, limits.ram};
    elfSpace.ccm = {T("CCM RAM"), ccmUsed, kPlusCartCcm};
    elfSpace.wholeFile = fileSize <= limits.ram;
    elfSpace.ram.used = elfSpace.wholeFile ? fileSize : bufferUsed;
  } else {
    elfSpace.flash = {T("Flash"), 0, limits.flash};
    elfSpace.ram = {T("RAM"), 0, limits.ram};
    if (chameleonImage) {
      // flash: what is stored there (code, constants, and the initial values of .data and
      // .RamFunc); RAM: everything placed at 0x2000_0000.
      for (auto& segment : chameleonImage->segments) {
        if (segment.type != LOAD) continue;
        if (segment.paddr >= 0x08000000 && segment.paddr < 0x08000000 + 0x80000) elfSpace.flash.used += segment.filesz;
        if (segment.vaddr >= 0x20000000 && segment.vaddr < 0x20000000 + 0x40000) elfSpace.ram.used += segment.memsz;
      }
      elfSpace.exact = true;
    } else {
      elfSpace.flash.used = elfSpace.code + elfSpace.ramCode + elfSpace.constants + elfSpace.data + kChameleonFirmwareFlash;
      elfSpace.ram.used = elfSpace.ramCode + elfSpace.data + elfSpace.bss + kChameleonFirmwareRam;
    }
  }
  for (auto* area : {&elfSpace.flash, &elfSpace.ram, &elfSpace.ccm}) {
    if (area->size <= 0) continue;
    int usedPercent = percent(area->used, area->size);
    std::string name = limits.cartridge == ElfCartridge::PlusCart ? F("PlusCart/UnoCart {0}", {area->name}) : F("Chameleon Cart {0}", {area->name});
    if (area->used > area->size)
      elfSpace.diagnostics.push_back({Diagnostic::Error, "", 0,
                               F("{0}: {1} bytes of {2}, it does not fit ({3} too many).",
                                 {name, std::to_string(area->used), std::to_string(area->size), std::to_string(area->used - area->size)})});
    else if (usedPercent >= warnPercent)
      elfSpace.diagnostics.push_back({Diagnostic::Warning, "", 0,
                               F("{0} is {1} % full ({2} of {3} bytes).",
                                 {name, std::to_string(usedPercent), std::to_string(area->used), std::to_string(area->size)})});
  }
  return elfSpace;
}

long gopherAceSram(long fileSize) {
  if (fileSize < 128000 - 64000) return 128000;
  if (fileSize < 256000 - 64000) return 256000;
  if (fileSize < 512000 - 64000) return 512000;
  return 64000;
}

ElfSpace analyzeAceSpace(const ElfFile& debugElf, long fileSize, int warnPercent) {
  enum : uint32_t { WRITE = 1, ALLOC = 2, EXEC = 4, NOBITS = 8 };
  ElfSpace aceSpace;
  aceSpace.fileSize = fileSize;
  aceSpace.cartridge = ElfCartridge::Ace;
  long sramEnd = 0;  // bytes from the start of SRAM: the firmware's arguments are below the program
  for (auto& section : debugElf.sections) {
    if (!(section.flags & ALLOC) || section.size == 0) continue;
    bool inSram = section.addr >= kAceSramStart && section.addr < kAceSramStart + 0x100000;
    if (!inSram) aceSpace.startCode += section.size;
    else if (section.flags & EXEC) aceSpace.code += section.size;
    else if (section.type == NOBITS) aceSpace.bss += section.size;
    else if (section.flags & WRITE) aceSpace.data += section.size;
    else aceSpace.constants += section.size;
    if (inSram) sramEnd = std::max(sramEnd, (long)(section.addr - kAceSramStart) + (long)section.size);
  }
  aceSpace.flash = {T("File"), fileSize, kAceFileLimit};
  aceSpace.ram = {T("SRAM"), sramEnd, kPlusCartBuffer};
  aceSpace.emulatorRam = {T("Gopher2600 SRAM"), sramEnd, gopherAceSram(fileSize)};
  for (auto* area : {&aceSpace.flash, &aceSpace.ram}) {
    int usedPercent = percent(area->used, area->size);
    std::string name = F("PlusCart/UnoCart {0}", {area->name});
    if (area->used > area->size)
      aceSpace.diagnostics.push_back({Diagnostic::Error, "", 0,
                                      F("{0}: {1} bytes of {2}, it does not fit ({3} too many).",
                                        {name, std::to_string(area->used), std::to_string(area->size), std::to_string(area->used - area->size)})});
    else if (usedPercent >= warnPercent)
      aceSpace.diagnostics.push_back({Diagnostic::Warning, "", 0,
                                      F("{0} is {1} % full ({2} of {3} bytes).",
                                        {name, std::to_string(usedPercent), std::to_string(area->used), std::to_string(area->size)})});
  }
  if (sramEnd > aceSpace.emulatorRam.size)
    aceSpace.diagnostics.push_back({Diagnostic::Warning, "", 0,
                                    F("Gopher2600 gives a file of {0} bytes only {1} bytes of SRAM, and the program needs {2}: it will not run there, though it may on the cartridge.",
                                      {std::to_string(fileSize), std::to_string(aceSpace.emulatorRam.size), std::to_string(sramEnd)})});
  return aceSpace;
}

std::string ElfSpace::summary() const {
  if (cartridge == ElfCartridge::Ace)
    return F("ACE file {0}/{1}  ·  SRAM {2}/{3}", {std::to_string(flash.used), std::to_string(flash.size), std::to_string(ram.used), std::to_string(ram.size)});
  if (cartridge == ElfCartridge::PlusCart)
    return F("PlusCart/UnoCart RAM {0}/{1}  ·  CCM RAM {2}/{3}",
             {std::to_string(ram.used), std::to_string(ram.size), std::to_string(ccm.used), std::to_string(ccm.size)});
  return F("Chameleon flash {0}/{1}  ·  RAM {2}/{3}{4}",
           {std::to_string(flash.used), std::to_string(flash.size), std::to_string(ram.used), std::to_string(ram.size),
            exact ? "" : std::string(" ") + T("(estimate)")});
}

}  // namespace vcs
