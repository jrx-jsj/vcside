// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "kb.h"

#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace vcs {

std::string upper(std::string text) {
  for (auto& character : text) character = (char)std::toupper((unsigned char)character);
  return text;
}

static int parseNumber(const std::string& text) {
  if (text.empty()) return -1;
  try {
    if (text[0] == '$') return std::stoi(text.substr(1), nullptr, 16);
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) return std::stoi(text.substr(2), nullptr, 16);
    return std::stoi(text);
  } catch (...) {
    return -1;
  }
}

int Register::addr() const { return parseNumber(address); }

std::vector<std::string> dasmDirectives() {
  return {"PROCESSOR", "ORG", "RORG", "SEG", "SEG.U", "INCLUDE", "INCBIN", "INCDIR", "EQU", "SET",
          "DC", "DC.B", "DC.W", "DS", "DS.B", "DS.W", "BYTE", ".BYTE", "WORD", ".WORD", "HEX",
          "ALIGN", "ECHO", "ERR", "IF", "IFCONST", "IFNCONST", "ELSE", "ENDIF", "EIF", "REPEAT",
          "REPEND", "MAC", "ENDM", "MEXIT", "SUBROUTINE", "LIST", "END", "VERTICAL_SYNC",
          "SLEEP", "CLEAN_START"};
}

static std::string stringValue(const json& object, const char* key) {
  auto found = object.find(key);
  if (found == object.end() || found->is_null()) return {};
  if (found->is_string()) return found->get<std::string>();
  return found->dump();
}

static std::vector<std::string> stringList(const json& object, const char* key) {
  std::vector<std::string> result;
  auto found = object.find(key);
  if (found != object.end() && found->is_array())
    for (auto& element : *found) result.push_back(element.is_string() ? element.get<std::string>() : element.dump());
  return result;
}

static int numberValue(const json& object, const char* key) {
  auto found = object.find(key);
  if (found == object.end()) return 0;
  if (found->is_number()) return found->get<int>();
  if (found->is_string()) return std::max(0, parseNumber(found->get<std::string>()));
  return 0;
}

static void readSources(const json& object, std::map<std::string, Source>& sources) {
  auto found = object.find("sources");
  if (found == object.end() || !found->is_array()) return;
  for (auto& source : *found)
    if (source.is_object()) sources[stringValue(source, "id")] = {stringValue(source, "id"), stringValue(source, "title"), stringValue(source, "author"), stringValue(source, "url")};
}

static bool readJson(const std::string& path, json& result, std::string* error) {
  std::ifstream stream(path);
  if (!stream) return false;  // missing file: not an error
  try {
    result = json::parse(stream);
    return true;
  } catch (const std::exception& exception) {
    if (error) *error += path + ": " + exception.what() + "\n";
    result = json();
    return false;
  }
}

bool KnowledgeBase::load(const std::string& dir, std::string* error) {
  bool ok = true;
  json cpuDocument;
  if (readJson(dir + "/6502.json", cpuDocument, error)) {
    cpuNotes_ = stringList(cpuDocument, "notes");
    readSources(cpuDocument, sources_);
    for (auto& opcodeJson : cpuDocument["opcodes"]) {
      Opcode opcode;
      opcode.code = parseNumber(stringValue(opcodeJson, "op"));
      opcode.mnemonic = stringValue(opcodeJson, "mnemonic");
      opcode.mode = stringValue(opcodeJson, "mode");
      opcode.bytes = numberValue(opcodeJson, "bytes");
      opcode.cycles = numberValue(opcodeJson, "cycles");
      opcode.pagePenalty = opcodeJson.value("page_penalty", false);
      opcode.branch = opcodeJson.value("branch", false);
      opcode.illegal = opcodeJson.value("illegal", false);
      opcode.stable = opcodeJson.value("stable", true);
      opcode.dasm = opcodeJson.value("dasm", true);
      opcodes_.push_back(opcode);
    }
    for (auto& [mnemonic, definition] : cpuDocument["instructions"].items()) {
      Instruction& instruction = instructions_[mnemonic];
      instruction.mnemonic = mnemonic;
      instruction.summary = stringValue(definition, "summary");
      instruction.flags = stringValue(definition, "flags");
      instruction.description = stringValue(definition, "description");
      instruction.illegal = definition.value("illegal", false);
      instruction.verified = definition.value("verified", true);
      instruction.sources = stringList(definition, "sources");
      instruction.aliases = stringList(definition, "aliases");
    }
    // Official opcodes first so that opcode(mn, mode) prefers them.
    std::stable_sort(opcodes_.begin(), opcodes_.end(),
                     [](const Opcode& left, const Opcode& right) { return !left.illegal && right.illegal; });
    for (auto& opcode : opcodes_) instructions_[opcode.mnemonic].opcodes.push_back(&opcode);
  } else if (!cpuDocument.is_null()) ok = false;

  for (const char* file : {"tia.json", "riot.json"}) {
    json chipDocument;
    if (!readJson(dir + "/" + file, chipDocument, error)) {
      if (!chipDocument.is_null()) ok = false;
      continue;
    }
    if (std::string(file) == "tia.json") tiaNotes_ = stringList(chipDocument, "notes");
    readSources(chipDocument, sources_);
    std::string chip = stringValue(chipDocument, "chip");
    for (auto& registerJson : chipDocument["registers"]) {
      Register reg;
      reg.chip = chip;
      reg.name = stringValue(registerJson, "name");
      reg.address = stringValue(registerJson, "address");
      reg.access = stringValue(registerJson, "access");
      reg.bits = stringValue(registerJson, "bits");
      reg.summary = stringValue(registerJson, "summary");
      reg.description = stringValue(registerJson, "description");
      reg.timing = stringValue(registerJson, "timing");
      reg.example = stringValue(registerJson, "example");
      reg.issues = stringList(registerJson, "issues");
      reg.sources = stringList(registerJson, "sources");
      reg.verified = registerJson.value("verified", true);
      registers_.push_back(reg);
    }
  }
  for (size_t i = 0; i < registers_.size(); i++) regIndex_.emplace(upper(registers_[i].name), i);

  json formatsDocument;
  if (readJson(dir + "/formats.json", formatsDocument, error)) {
    readSources(formatsDocument, sources_);
    for (auto& formatJson : formatsDocument["formats"]) {
      CartFormat format;
      format.id = stringValue(formatJson, "id");
      format.name = stringValue(formatJson, "name");
      format.category = stringValue(formatJson, "category");
      format.size = stringValue(formatJson, "size");
      format.description = stringValue(formatJson, "description");
      format.stella = stringValue(formatJson, "stella");
      format.gopher2600 = stringValue(formatJson, "gopher2600");
      format.detection = stringValue(formatJson, "detection");
      format.ram = stringValue(formatJson, "ram");
      format.hotspots = stringList(formatJson, "hotspots");
      format.issues = stringList(formatJson, "issues");
      format.sources = stringList(formatJson, "sources");
      format.verified = formatJson.value("verified", true);
      if (formatJson.contains("space") && formatJson["space"].is_object()) {
        auto& spaceJson = formatJson["space"];
        format.space.known = true;
        format.space.romSizes = spaceJson.value("rom_sizes", std::vector<long>{});
        format.space.bankSize = spaceJson.value("bank_size", 4096L);
        format.space.ramBytes = spaceJson.value("ram_bytes", 0L);
        for (auto& range : spaceJson.value("reserved", std::vector<std::string>{}))
          if (auto parsedRange = parseRange(range)) format.space.reserved.push_back(*parsedRange);
        for (auto& range : spaceJson.value("ram_windows", std::vector<std::string>{}))
          if (auto parsedRange = parseRange(range)) format.space.ramWindows.push_back(*parsedRange);
      }
      formats_.push_back(format);
    }
  }
  json tvDocument;
  if (readJson(dir + "/tv.json", tvDocument, error)) {
    readSources(tvDocument, sources_);
    for (auto& standardJson : tvDocument["standards"]) {
      TvStandard standard;
      standard.id = stringValue(standardJson, "id");
      standard.linesTotal = numberValue(standardJson, "lines_total");
      standard.vsync = numberValue(standardJson, "vsync");
      standard.vblank = numberValue(standardJson, "vblank");
      standard.visible = numberValue(standardJson, "visible");
      standard.overscan = numberValue(standardJson, "overscan");
      standard.fps = stringValue(standardJson, "fps");
      standard.palette = stringValue(standardJson, "palette");
      standard.stellaTv = stringValue(standardJson, "stella_tv");
      standard.notes = stringList(standardJson, "notes");
      standard.sources = stringList(standardJson, "sources");
      standard.verified = standardJson.value("verified", true);
      tv_.push_back(standard);
    }
  }
  json emulatorsDocument;
  if (readJson(dir + "/emulators.json", emulatorsDocument, error)) {
    readSources(emulatorsDocument, sources_);
    if (emulatorsDocument.contains("emulators") && emulatorsDocument["emulators"].is_object())
      for (auto& [id, emulatorJson] : emulatorsDocument["emulators"].items()) {
        EmulatorInfo info;
        info.id = id;
        info.name = stringValue(emulatorJson, "name");
        for (auto& optionJson : emulatorJson["options"]) {
          EmuOption option;
          option.id = stringValue(optionJson, "id");
          option.group = stringValue(optionJson, "group");
          option.label = stringValue(optionJson, "label");
          option.description = stringValue(optionJson, "description");
          option.type = stringValue(optionJson, "type");
          option.arg = stringValue(optionJson, "arg");
          option.noValue = optionJson.value("no_value", false);
          option.developer = optionJson.value("developer", false);
          option.flag = stringValue(optionJson, "flag");
          option.pref = stringValue(optionJson, "pref");
          option.min = optionJson.value("min", 0);
          option.max = optionJson.value("max", 0);
          if (optionJson.contains("values"))
            for (auto& valueJson : optionJson["values"]) option.values.push_back({stringValue(valueJson, "value"), stringValue(valueJson, "label")});
          if (optionJson.contains("i18n") && optionJson["i18n"].is_object())
            for (auto& [language, translation] : optionJson["i18n"].items()) option.i18n[language] = {stringValue(translation, "label"), stringValue(translation, "description")};
          info.options.push_back(option);
        }
        emulators_.push_back(info);
      }
  }
  json basicDocument;
  if (readJson(dir + "/batari-basic.json", basicDocument, error)) {
    readSources(basicDocument, sources_);
    if (basicDocument.contains("keywords") && basicDocument["keywords"].is_array())
      for (auto& keywordJson : basicDocument["keywords"]) {
        BasicKeyword keyword;
        keyword.word = stringValue(keywordJson, "word");
        keyword.kind = stringValue(keywordJson, "kind");
        keyword.syntax = stringValue(keywordJson, "syntax");
        keyword.summary = stringValue(keywordJson, "summary");
        keyword.sources = stringList(keywordJson, "sources");
        if (keywordJson.contains("i18n") && keywordJson["i18n"].is_object())
          for (auto& [language, translation] : keywordJson["i18n"].items())
            if (translation.is_string()) keyword.i18n[language] = translation.get<std::string>();
        // "joyNup" with "range": [0, 3] is joy0up ... joy3up
        auto markerPosition = keyword.word.find('N');
        if (markerPosition != std::string::npos && keywordJson.contains("range") && keywordJson["range"].is_array() && keywordJson["range"].size() == 2) {
          for (int i = keywordJson["range"][0].get<int>(); i <= keywordJson["range"][1].get<int>(); i++) {
            BasicKeyword one = keyword;
            // N after a lowercase letter (joyNup) or standing alone ("joystick N")
            auto put = [&](std::string& text) {
              auto alnum = [&](size_t position) { return position < text.size() && std::isalnum((unsigned char)text[position]); };
              for (size_t position = 0; (position = text.find('N', position)) != std::string::npos; position++) {
                bool after = position > 0 && std::islower((unsigned char)text[position - 1]);
                bool alone = (position == 0 || !alnum(position - 1)) && !alnum(position + 1);
                if (after || alone) text.replace(position, 1, std::to_string(i));
              }
            };
            put(one.word);
            put(one.syntax);
            put(one.summary);
            for (auto& [language, translation] : one.i18n) put(translation);
            basic_[one.word] = one;
          }
        } else {
          basic_[keyword.word] = keyword;
        }
      }
  }
  return ok && (!error || error->empty());
}

const EmulatorInfo* KnowledgeBase::emulator(const std::string& id) const {
  for (auto& emulator : emulators_)
    if (emulator.id == id) return &emulator;
  return nullptr;
}

const EmuOption* EmulatorInfo::option(const std::string& optionId) const {
  for (auto& emulatorOption : options)
    if (emulatorOption.id == optionId) return &emulatorOption;
  return nullptr;
}

// "pt-BR" falls back to "pt", then to English.
static const std::pair<std::string, std::string>* translation(const EmuOption& option) {
  std::string language = currentLanguage();
  for (;;) {
    auto found = option.i18n.find(language);
    if (found != option.i18n.end()) return &found->second;
    size_t dash = language.find_last_of("-_");
    if (dash == std::string::npos) return nullptr;
    language.resize(dash);
  }
}

const std::string& EmuOption::labelText() const {
  auto* entry = translation(*this);
  return entry && !entry->first.empty() ? entry->first : label;
}

const std::string& EmuOption::descriptionText() const {
  auto* entry = translation(*this);
  return entry && !entry->second.empty() ? entry->second : description;
}

const std::string& BasicKeyword::summaryText() const {
  std::string language = currentLanguage();
  for (;;) {
    auto found = i18n.find(language);
    if (found != i18n.end() && !found->second.empty()) return found->second;
    size_t dash = language.find_last_of("-_");
    if (dash == std::string::npos) return summary;
    language.resize(dash);
  }
}

const BasicKeyword* KnowledgeBase::basic(const std::string& word) const {
  auto found = basic_.find(word);
  return found == basic_.end() ? nullptr : &found->second;
}

const Source* KnowledgeBase::source(const std::string& id) const {
  auto found = sources_.find(id);
  return found == sources_.end() ? nullptr : &found->second;
}

const Instruction* KnowledgeBase::instruction(const std::string& mnemonic) const {
  auto found = instructions_.find(upper(mnemonic));
  return found == instructions_.end() ? nullptr : &found->second;
}

const Opcode* KnowledgeBase::opcode(const std::string& mnemonic, const std::string& mode) const {
  const Instruction* found = instruction(mnemonic);
  if (!found) return nullptr;
  for (auto* opcode : found->opcodes)
    if (opcode->mode == mode) return opcode;
  return nullptr;
}

const Register* KnowledgeBase::reg(const std::string& name) const {
  auto found = regIndex_.find(upper(name));
  return found == regIndex_.end() ? nullptr : &registers_[found->second];
}

const CartFormat* KnowledgeBase::format(const std::string& id) const {
  for (auto& format : formats_)
    if (upper(format.id) == upper(id)) return &format;
  return nullptr;
}

const TvStandard* KnowledgeBase::tv(const std::string& id) const {
  for (auto& standard : tv_)
    if (upper(standard.id) == upper(id)) return &standard;
  return nullptr;
}

std::vector<KbEntry> KnowledgeBase::complete(const std::string& prefix, size_t max) const {
  std::vector<KbEntry> result;
  if (prefix.empty()) return result;
  std::string upperPrefix = upper(prefix);
  auto starts = [&](const std::string& word) { return upper(word).compare(0, upperPrefix.size(), upperPrefix) == 0; };
  for (auto& [mnemonic, instruction] : instructions_)
    if (starts(mnemonic) && result.size() < max) result.push_back({KbEntry::Mnemonic, mnemonic, instruction.summary});
  for (auto& registerEntry : registers_)
    if (starts(registerEntry.name) && result.size() < max) result.push_back({KbEntry::Register, registerEntry.name, registerEntry.summary});
  for (auto& directive : dasmDirectives())
    if (starts(directive) && result.size() < max) result.push_back({KbEntry::Directive, directive, "DASM directive"});
  return result;
}

bool naturalLess(const std::string& left, const std::string& right) {
  size_t i = 0, j = 0;
  while (i < left.size() && j < right.size()) {
    if (std::isdigit((unsigned char)left[i]) && std::isdigit((unsigned char)right[j])) {
      size_t leftEnd = i, rightEnd = j;
      while (leftEnd < left.size() && std::isdigit((unsigned char)left[leftEnd])) leftEnd++;
      while (rightEnd < right.size() && std::isdigit((unsigned char)right[rightEnd])) rightEnd++;
      // compare by length without leading zeros, then digit by digit
      size_t leftSignificant = i, rightSignificant = j;
      while (leftSignificant + 1 < leftEnd && left[leftSignificant] == '0') leftSignificant++;
      while (rightSignificant + 1 < rightEnd && right[rightSignificant] == '0') rightSignificant++;
      if (leftEnd - leftSignificant != rightEnd - rightSignificant) return leftEnd - leftSignificant < rightEnd - rightSignificant;
      int comparison = left.compare(leftSignificant, leftEnd - leftSignificant, right, rightSignificant, rightEnd - rightSignificant);
      if (comparison != 0) return comparison < 0;
      i = leftEnd, j = rightEnd;
      continue;
    }
    int leftChar = std::tolower((unsigned char)left[i]), rightChar = std::tolower((unsigned char)right[j]);
    if (leftChar != rightChar) return leftChar < rightChar;
    i++, j++;
  }
  return left.size() - i < right.size() - j;
}

std::vector<std::pair<std::string, std::string>> formatCategories() {
  return {{"atari", T("Atari")},
          {"classic", T("Other makers of the time")},
          {"homebrew", T("Modern bank switching")},
          {"arm", T("ARM cartridges (Harmony, PlusCart, Chameleon...)")}};
}

}  // namespace vcs
