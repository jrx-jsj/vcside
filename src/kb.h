// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// Knowledge base: 6502 opcodes, TIA/RIOT registers, cartridge formats, TV standards.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "format_space.h"

namespace vcs {

struct Opcode {
  int code = 0;
  std::string mnemonic, mode;
  int bytes = 0, cycles = 0;
  bool pagePenalty = false, branch = false, illegal = false, stable = true;
  bool dasm = true;  // false: DASM has no mnemonic for it, write it with .byte
};

struct Instruction {
  std::string mnemonic, summary, flags, description;
  bool illegal = false, verified = true;
  std::vector<std::string> sources, aliases;
  std::vector<const Opcode*> opcodes;
};

struct Register {
  std::string chip, name, address, access, bits, summary, description, timing, example;
  std::vector<std::string> issues, sources;
  bool verified = true;
  int addr() const;
};

struct CartFormat {
  std::string id, name, size, description, stella, gopher2600, detection, ram;
  std::vector<std::string> hotspots, issues, sources;
  bool verified = true;
  FormatSpace space;  // sizes for the space warnings (space.known = false if not given)
};

struct TvStandard {
  std::string id, stellaTv, palette;
  int linesTotal = 0, vsync = 0, vblank = 0, visible = 0, overscan = 0;
  std::string fps;
  std::vector<std::string> notes, sources;
  bool verified = true;
};

// Where a fact comes from (data/kb/*.json "sources" arrays; docs/fuentes.md).
struct Source {
  std::string id, title, author, url;
};

// A word the editor can complete or explain.
struct KbEntry {
  enum Kind { Mnemonic, Register, Directive, Symbol } kind;
  std::string word, summary;
};

class KnowledgeBase {
 public:
  // Loads every *.json under dir; missing files are tolerated. Returns false
  // and fills error if a present file is malformed.
  bool load(const std::string& dir, std::string* error = nullptr);

  const Instruction* instruction(const std::string& mnemonic) const;
  const Opcode* opcode(const std::string& mnemonic, const std::string& mode) const;
  const Register* reg(const std::string& name) const;
  const CartFormat* format(const std::string& id) const;
  const TvStandard* tv(const std::string& id) const;
  const Source* source(const std::string& id) const;

  const std::map<std::string, Instruction>& instructions() const { return instructions_; }
  const std::vector<Register>& registers() const { return registers_; }
  const std::vector<CartFormat>& formats() const { return formats_; }
  const std::vector<TvStandard>& tvStandards() const { return tv_; }
  const std::vector<std::string>& cpuNotes() const { return cpuNotes_; }
  const std::vector<std::string>& tiaNotes() const { return tiaNotes_; }

  // Completion candidates whose word starts with prefix (case-insensitive).
  std::vector<KbEntry> complete(const std::string& prefix, size_t max = 30) const;

 private:
  std::vector<Opcode> opcodes_;
  std::map<std::string, Instruction> instructions_;
  std::vector<Register> registers_;
  std::map<std::string, size_t> regIndex_;
  std::vector<CartFormat> formats_;
  std::vector<TvStandard> tv_;
  std::vector<std::string> cpuNotes_, tiaNotes_;
  std::map<std::string, Source> sources_;
};

std::string upper(std::string s);
std::vector<std::string> dasmDirectives();

}  // namespace vcs
