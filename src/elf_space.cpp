// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "elf_space.h"

#include <algorithm>

#include "i18n.h"

namespace vcs {

static uint32_t u32(const std::string& d, size_t at) {
  if (at + 4 > d.size()) return 0;
  return (uint32_t)(uint8_t)d[at] | (uint32_t)(uint8_t)d[at + 1] << 8 | (uint32_t)(uint8_t)d[at + 2] << 16 |
         (uint32_t)(uint8_t)d[at + 3] << 24;
}
static uint32_t u16(const std::string& d, size_t at) {
  if (at + 2 > d.size()) return 0;
  return (uint32_t)(uint8_t)d[at] | (uint32_t)(uint8_t)d[at + 1] << 8;
}

bool readElf(const std::string& d, ElfFile& out, std::string* error) {
  out = ElfFile();
  if (d.size() < 52 || d.compare(0, 4, "\x7f" "ELF") != 0 || d[4] != 1 || d[5] != 1) {
    if (error) *error = T("not a 32-bit little-endian ELF file");
    return false;
  }
  out.type = (int)u16(d, 16);
  out.machine = (int)u16(d, 18);
  uint32_t phoff = u32(d, 28), shoff = u32(d, 32);
  uint32_t phentsize = u16(d, 42), phnum = u16(d, 44), shentsize = u16(d, 46), shnum = u16(d, 48), shstrndx = u16(d, 50);
  for (uint32_t i = 0; i < phnum && phentsize >= 32; i++) {
    size_t p = phoff + (size_t)i * phentsize;
    if (p + 32 > d.size()) break;
    out.segments.push_back({u32(d, p), u32(d, p + 8), u32(d, p + 12), u32(d, p + 16), u32(d, p + 20)});
  }
  std::vector<uint32_t> nameOff;
  for (uint32_t i = 0; i < shnum && shentsize >= 40; i++) {
    size_t p = shoff + (size_t)i * shentsize;
    if (p + 40 > d.size()) break;
    ElfFile::Section s;
    s.type = u32(d, p + 4);
    s.flags = u32(d, p + 8);
    s.addr = u32(d, p + 12);
    s.size = u32(d, p + 20);
    nameOff.push_back(u32(d, p));
    out.sections.push_back(s);
  }
  if (shstrndx < out.sections.size()) {
    size_t strOff = u32(d, shoff + (size_t)shstrndx * shentsize + 16);
    for (size_t i = 0; i < out.sections.size(); i++) {
      size_t at = strOff + nameOff[i];
      if (at < d.size()) out.sections[i].name = d.c_str() + at;
    }
  }
  return true;
}

static int percent(long used, long size) { return size > 0 ? (int)(used * 100 / size) : 0; }

ElfSpace analyzeElfSpace(const ElfFile& rom, long fileSize, const ElfFile* cc, int warnPercent) {
  enum : uint32_t { WRITE = 1, ALLOC = 2, EXEC = 4, NOBITS = 8, LOAD = 1 };
  ElfSpace r;
  r.fileSize = fileSize;
  for (auto& s : rom.sections) {
    if (!(s.flags & ALLOC)) continue;
    if (s.name.rfind(".RamFunc", 0) == 0) r.ramCode += s.size;
    else if (s.flags & EXEC) r.code += s.size;
    else if (s.type == NOBITS) r.bss += s.size;
    else if (s.flags & WRITE) r.data += s.size;
    else r.constants += s.size;
  }
  r.flash = {T("Flash"), 0, kChameleonFlash};
  r.ram = {T("RAM"), 0, kChameleonRam};
  if (cc) {
    // flash: what is stored there (code, constants, and the initial values of .data and
    // .RamFunc); RAM: everything placed at 0x2000_0000.
    for (auto& g : cc->segments) {
      if (g.type != LOAD) continue;
      if (g.paddr >= 0x08000000 && g.paddr < 0x08000000 + 0x80000) r.flash.used += g.filesz;
      if (g.vaddr >= 0x20000000 && g.vaddr < 0x20000000 + 0x40000) r.ram.used += g.memsz;
    }
    r.exact = true;
  } else {
    r.flash.used = r.code + r.ramCode + r.constants + r.data + kChameleonFirmwareFlash;
    r.ram.used = r.ramCode + r.data + r.bss + kChameleonFirmwareRam;
  }
  for (auto* a : {&r.flash, &r.ram}) {
    int p = percent(a->used, a->size);
    std::string name = F("Chameleon Cart {0}", {a->name});
    if (a->used > a->size)
      r.diagnostics.push_back({Diagnostic::Error, "", 0,
                               F("{0}: {1} bytes of {2}, it does not fit ({3} too many).",
                                 {name, std::to_string(a->used), std::to_string(a->size), std::to_string(a->used - a->size)})});
    else if (p >= warnPercent)
      r.diagnostics.push_back({Diagnostic::Warning, "", 0,
                               F("{0} is {1} % full ({2} of {3} bytes).",
                                 {name, std::to_string(p), std::to_string(a->used), std::to_string(a->size)})});
  }
  return r;
}

std::string ElfSpace::summary() const {
  return F("Chameleon flash {0}/{1}  ·  RAM {2}/{3}{4}",
           {std::to_string(flash.used), std::to_string(flash.size), std::to_string(ram.used), std::to_string(ram.size),
            exact ? "" : std::string(" ") + T("(estimate)")});
}

}  // namespace vcs
