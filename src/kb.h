// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Knowledge base: 6502 opcodes, TIA/RIOT registers (and MARIA's, for 7800 ELF games), cartridge formats, TV standards.
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

// One line of a register's bit table: "7-4", "P0 joystick", "right, left, down, up; 0 = pressed".
struct BitField {
  std::string bits, name, meaning;
};

struct Register {
  std::string chip, name, address, access, bits, summary, description, timing, example;
  std::vector<std::string> issues, sources;
  std::vector<BitField> fields;    // the bits, as a table (optional)
  std::vector<std::string> seeAlso;  // registers, instructions or recipe ids worth reading next (optional)
  bool verified = true;
  int addr() const;
};

struct CartFormat {
  std::string id, name, size, description, stella, gopher2600, detection, ram;
  std::string category;  // atari, classic, homebrew, arm
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

// A launch option of an emulator (data/kb/emulators.json). Values are strings as
// the emulator takes them; an option left unset is not passed at all.
struct EmuChoice {
  std::string value, label;
};
struct EmuOption {
  std::string id, group, label, description;
  std::string type;          // bool, choice, int, text
  std::string arg;           // Stella: "-tv.filter"
  bool noValue = false;      // Stella: an option without value (-holdreset); "1" = pass it
  bool developer = false;    // Stella: needs -dev.settings 1
  std::string flag;          // Gopher2600: command-line flag ("-left")
  std::string pref;          // Gopher2600: preferences key ("hardware.randState")
  int min = 0, max = 0;      // int: range (max 0 = none)
  std::vector<EmuChoice> values;
  std::map<std::string, std::pair<std::string, std::string>> i18n;  // language -> label, description
  // In the interface language (English if there is no translation).
  const std::string& labelText() const;
  const std::string& descriptionText() const;
};
struct EmulatorInfo {
  std::string id, name;
  std::vector<EmuOption> options;
  const EmuOption* option(const std::string& optionId) const;
};

// Where a fact comes from (data/kb/*.json "sources" arrays; docs/sources.md).
struct Source {
  std::string id, title, author, url;
};

// A word of a language the editor can complete or explain: batari Basic (data/kb/batari-basic.json),
// TinyELF Basic (tinyelf-basic.json) or the C of ELF games (elf.json, vcsLib): statement, control,
// directive, block, function, variable, operator, command, macro or comment.
struct BasicKeyword {
  std::string word, kind, syntax, summary, description, example;
  std::map<std::string, std::string> i18n;             // language -> summary
  std::map<std::string, std::string> i18nDescription;  // language -> description
  std::vector<std::string> sources, seeAlso;
  bool available = true;  // false: the language knows the word but this build leaves it out (TinyELF's PLAY...)
  const std::string& summaryText() const;
  const std::string& descriptionText() const;
};

// The words of one language, and what the knowledge base says of it as a whole.
struct LanguageWords {
  std::string id, title;  // "bb" "batari Basic", "tinyelf" "TinyELF Basic", "elf" "ELF"
  bool caseSensitive = true;  // TinyELF's words are kept in capitals and found in any case
  std::vector<std::string> notes;
  std::map<std::string, BasicKeyword> words;  // by word; "joyNup" with a range is joy0up ... joy3up
  std::vector<BasicKeyword> entries;          // as the file has them ("joyNup" once), for the documentation
  std::map<std::string, std::string> entryOf;  // a numbered word -> its entry's word (joy2up -> joyNup)
  // An entry by its word (joyNup) or a word of it (joy0up): what the documentation shows.
  const BasicKeyword* entry(const std::string& word) const;
};

// A how-to entry (data/kb/techniques.json): what a programmer wants to do, with the registers and the steps.
struct Technique {
  std::string id, title, summary, example, note;
  std::vector<std::string> keywords, steps, related, sources;
  bool verified = true;
};

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
  const EmulatorInfo* emulator(const std::string& id) const;
  // batari Basic word (case-sensitive, as the compiler); "player0:" with its colon.
  const BasicKeyword* basic(const std::string& word) const;
  const std::map<std::string, BasicKeyword>& basicKeywords() const { return basic_; }
  // The languages whose words are documented, in the order the documentation lists them.
  static const std::vector<std::string>& languageIds();
  const LanguageWords* language(const std::string& id) const;
  // A word of a language: as it is written (bb, elf) or in any case (tinyelf).
  const BasicKeyword* languageWord(const std::string& languageId, const std::string& word) const;

  const std::map<std::string, Instruction>& instructions() const { return instructions_; }
  const std::vector<Register>& registers() const { return registers_; }
  const std::vector<CartFormat>& formats() const { return formats_; }
  const std::vector<TvStandard>& tvStandards() const { return tv_; }
  const std::vector<std::string>& cpuNotes() const { return cpuNotes_; }
  const std::map<std::string, Source>& sources() const { return sources_; }
  const std::vector<std::string>& tiaNotes() const { return tiaNotes_; }
  // The Atari 7800 as a Chameleon Cart ELF game reaches it (data/kb/a7800.json).
  const std::vector<std::string>& a7800Notes() const { return a7800Notes_; }
  const std::vector<Technique>& techniques() const { return techniques_; }

  // Completion candidates whose word starts with prefix (case-insensitive).
  std::vector<KbEntry> complete(const std::string& prefix, size_t max = 30) const;

 private:
  std::vector<Opcode> opcodes_;
  std::map<std::string, Instruction> instructions_;
  std::vector<Register> registers_;
  std::map<std::string, size_t> regIndex_;
  std::vector<CartFormat> formats_;
  std::vector<TvStandard> tv_;
  std::vector<EmulatorInfo> emulators_;
  std::vector<std::string> cpuNotes_, tiaNotes_, a7800Notes_;
  std::vector<Technique> techniques_;
  std::map<std::string, Source> sources_;
  std::map<std::string, BasicKeyword> basic_;  // = languages_["bb"].words
  std::map<std::string, LanguageWords> languages_;
  bool loadLanguage(const std::string& path, const std::string& id, const std::string& title, bool caseSensitive, std::string* error);
};

std::string upper(std::string text);
// Compares text with the numbers in it as numbers ("8K" < "16K"), ignoring case.
bool naturalLess(const std::string& left, const std::string& right);
// Format categories in menu order, with their titles (translated).
std::vector<std::pair<std::string, std::string>> formatCategories();
std::vector<std::string> dasmDirectives();

}  // namespace vcs
