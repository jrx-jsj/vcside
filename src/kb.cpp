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

std::string upper(std::string s) {
  for (auto& c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}

static int parseNumber(const std::string& s) {
  if (s.empty()) return -1;
  try {
    if (s[0] == '$') return std::stoi(s.substr(1), nullptr, 16);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return std::stoi(s.substr(2), nullptr, 16);
    return std::stoi(s);
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

static std::string str(const json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return {};
  if (it->is_string()) return it->get<std::string>();
  return it->dump();
}

static std::vector<std::string> strs(const json& j, const char* key) {
  std::vector<std::string> out;
  auto it = j.find(key);
  if (it != j.end() && it->is_array())
    for (auto& v : *it) out.push_back(v.is_string() ? v.get<std::string>() : v.dump());
  return out;
}

static int num(const json& j, const char* key) {
  auto it = j.find(key);
  if (it == j.end()) return 0;
  if (it->is_number()) return it->get<int>();
  if (it->is_string()) return std::max(0, parseNumber(it->get<std::string>()));
  return 0;
}

static void readSources(const json& j, std::map<std::string, Source>& out) {
  auto it = j.find("sources");
  if (it == j.end() || !it->is_array()) return;
  for (auto& s : *it)
    if (s.is_object()) out[str(s, "id")] = {str(s, "id"), str(s, "title"), str(s, "author"), str(s, "url")};
}

static bool readJson(const std::string& path, json& out, std::string* error) {
  std::ifstream f(path);
  if (!f) return false;  // missing file: not an error
  try {
    out = json::parse(f);
    return true;
  } catch (const std::exception& e) {
    if (error) *error += path + ": " + e.what() + "\n";
    out = json();
    return false;
  }
}

bool KnowledgeBase::load(const std::string& dir, std::string* error) {
  bool ok = true;
  json j;
  if (readJson(dir + "/6502.json", j, error)) {
    cpuNotes_ = strs(j, "notes");
    readSources(j, sources_);
    for (auto& o : j["opcodes"]) {
      Opcode op;
      op.code = parseNumber(str(o, "op"));
      op.mnemonic = str(o, "mnemonic");
      op.mode = str(o, "mode");
      op.bytes = num(o, "bytes");
      op.cycles = num(o, "cycles");
      op.pagePenalty = o.value("page_penalty", false);
      op.branch = o.value("branch", false);
      op.illegal = o.value("illegal", false);
      op.stable = o.value("stable", true);
      op.dasm = o.value("dasm", true);
      opcodes_.push_back(op);
    }
    for (auto& [mn, d] : j["instructions"].items()) {
      Instruction& in = instructions_[mn];
      in.mnemonic = mn;
      in.summary = str(d, "summary");
      in.flags = str(d, "flags");
      in.description = str(d, "description");
      in.illegal = d.value("illegal", false);
      in.verified = d.value("verified", true);
      in.sources = strs(d, "sources");
      in.aliases = strs(d, "aliases");
    }
    // Official opcodes first so that opcode(mn, mode) prefers them.
    std::stable_sort(opcodes_.begin(), opcodes_.end(),
                     [](const Opcode& a, const Opcode& b) { return !a.illegal && b.illegal; });
    for (auto& op : opcodes_) instructions_[op.mnemonic].opcodes.push_back(&op);
  } else if (!j.is_null()) ok = false;

  for (const char* file : {"tia.json", "riot.json"}) {
    json r;
    if (!readJson(dir + "/" + file, r, error)) {
      if (!r.is_null()) ok = false;
      continue;
    }
    if (std::string(file) == "tia.json") tiaNotes_ = strs(r, "notes");
    readSources(r, sources_);
    std::string chip = str(r, "chip");
    for (auto& d : r["registers"]) {
      Register reg;
      reg.chip = chip;
      reg.name = str(d, "name");
      reg.address = str(d, "address");
      reg.access = str(d, "access");
      reg.bits = str(d, "bits");
      reg.summary = str(d, "summary");
      reg.description = str(d, "description");
      reg.timing = str(d, "timing");
      reg.example = str(d, "example");
      reg.issues = strs(d, "issues");
      reg.sources = strs(d, "sources");
      reg.verified = d.value("verified", true);
      registers_.push_back(reg);
    }
  }
  for (size_t i = 0; i < registers_.size(); i++) regIndex_.emplace(upper(registers_[i].name), i);

  json f;
  if (readJson(dir + "/formats.json", f, error)) {
    readSources(f, sources_);
    for (auto& d : f["formats"]) {
      CartFormat c;
      c.id = str(d, "id");
      c.name = str(d, "name");
      c.category = str(d, "category");
      c.size = str(d, "size");
      c.description = str(d, "description");
      c.stella = str(d, "stella");
      c.gopher2600 = str(d, "gopher2600");
      c.detection = str(d, "detection");
      c.ram = str(d, "ram");
      c.hotspots = strs(d, "hotspots");
      c.issues = strs(d, "issues");
      c.sources = strs(d, "sources");
      c.verified = d.value("verified", true);
      if (d.contains("space") && d["space"].is_object()) {
        auto& sp = d["space"];
        c.space.known = true;
        c.space.romSizes = sp.value("rom_sizes", std::vector<long>{});
        c.space.bankSize = sp.value("bank_size", 4096L);
        c.space.ramBytes = sp.value("ram_bytes", 0L);
        for (auto& r : sp.value("reserved", std::vector<std::string>{}))
          if (auto x = parseRange(r)) c.space.reserved.push_back(*x);
        for (auto& r : sp.value("ram_windows", std::vector<std::string>{}))
          if (auto x = parseRange(r)) c.space.ramWindows.push_back(*x);
      }
      formats_.push_back(c);
    }
  }
  json t;
  if (readJson(dir + "/tv.json", t, error)) {
    readSources(t, sources_);
    for (auto& d : t["standards"]) {
      TvStandard s;
      s.id = str(d, "id");
      s.linesTotal = num(d, "lines_total");
      s.vsync = num(d, "vsync");
      s.vblank = num(d, "vblank");
      s.visible = num(d, "visible");
      s.overscan = num(d, "overscan");
      s.fps = str(d, "fps");
      s.palette = str(d, "palette");
      s.stellaTv = str(d, "stella_tv");
      s.notes = strs(d, "notes");
      s.sources = strs(d, "sources");
      s.verified = d.value("verified", true);
      tv_.push_back(s);
    }
  }
  json e;
  if (readJson(dir + "/emulators.json", e, error)) {
    readSources(e, sources_);
    if (e.contains("emulators") && e["emulators"].is_object())
      for (auto& [id, d] : e["emulators"].items()) {
        EmulatorInfo info;
        info.id = id;
        info.name = str(d, "name");
        for (auto& o : d["options"]) {
          EmuOption op;
          op.id = str(o, "id");
          op.group = str(o, "group");
          op.label = str(o, "label");
          op.description = str(o, "description");
          op.type = str(o, "type");
          op.arg = str(o, "arg");
          op.noValue = o.value("no_value", false);
          op.developer = o.value("developer", false);
          op.flag = str(o, "flag");
          op.pref = str(o, "pref");
          op.min = o.value("min", 0);
          op.max = o.value("max", 0);
          if (o.contains("values"))
            for (auto& v : o["values"]) op.values.push_back({str(v, "value"), str(v, "label")});
          if (o.contains("i18n") && o["i18n"].is_object())
            for (auto& [lang, t] : o["i18n"].items()) op.i18n[lang] = {str(t, "label"), str(t, "description")};
          info.options.push_back(op);
        }
        emulators_.push_back(info);
      }
  }
  json b;
  if (readJson(dir + "/batari-basic.json", b, error)) {
    readSources(b, sources_);
    if (b.contains("keywords") && b["keywords"].is_array())
      for (auto& k : b["keywords"]) {
        BasicKeyword w;
        w.word = str(k, "word");
        w.kind = str(k, "kind");
        w.syntax = str(k, "syntax");
        w.summary = str(k, "summary");
        w.sources = strs(k, "sources");
        if (k.contains("i18n") && k["i18n"].is_object())
          for (auto& [lang, t] : k["i18n"].items())
            if (t.is_string()) w.i18n[lang] = t.get<std::string>();
        // "joyNup" with "range": [0, 3] is joy0up ... joy3up
        auto n = w.word.find('N');
        if (n != std::string::npos && k.contains("range") && k["range"].is_array() && k["range"].size() == 2) {
          for (int i = k["range"][0].get<int>(); i <= k["range"][1].get<int>(); i++) {
            BasicKeyword one = w;
            // N after a lowercase letter (joyNup) or standing alone ("joystick N")
            auto put = [&](std::string& t) {
              auto alnum = [&](size_t at) { return at < t.size() && std::isalnum((unsigned char)t[at]); };
              for (size_t at = 0; (at = t.find('N', at)) != std::string::npos; at++) {
                bool after = at > 0 && std::islower((unsigned char)t[at - 1]);
                bool alone = (at == 0 || !alnum(at - 1)) && !alnum(at + 1);
                if (after || alone) t.replace(at, 1, std::to_string(i));
              }
            };
            put(one.word);
            put(one.syntax);
            put(one.summary);
            for (auto& [lang, t] : one.i18n) put(t);
            basic_[one.word] = one;
          }
        } else {
          basic_[w.word] = w;
        }
      }
  }
  return ok && (!error || error->empty());
}

const EmulatorInfo* KnowledgeBase::emulator(const std::string& id) const {
  for (auto& e : emulators_)
    if (e.id == id) return &e;
  return nullptr;
}

const EmuOption* EmulatorInfo::option(const std::string& id) const {
  for (auto& o : options)
    if (o.id == id) return &o;
  return nullptr;
}

// "pt-BR" falls back to "pt", then to English.
static const std::pair<std::string, std::string>* translation(const EmuOption& o) {
  std::string lang = currentLanguage();
  for (;;) {
    auto it = o.i18n.find(lang);
    if (it != o.i18n.end()) return &it->second;
    size_t dash = lang.find_last_of("-_");
    if (dash == std::string::npos) return nullptr;
    lang.resize(dash);
  }
}

const std::string& EmuOption::labelText() const {
  auto* t = translation(*this);
  return t && !t->first.empty() ? t->first : label;
}

const std::string& EmuOption::descriptionText() const {
  auto* t = translation(*this);
  return t && !t->second.empty() ? t->second : description;
}

const std::string& BasicKeyword::summaryText() const {
  std::string lang = currentLanguage();
  for (;;) {
    auto it = i18n.find(lang);
    if (it != i18n.end() && !it->second.empty()) return it->second;
    size_t dash = lang.find_last_of("-_");
    if (dash == std::string::npos) return summary;
    lang.resize(dash);
  }
}

const BasicKeyword* KnowledgeBase::basic(const std::string& word) const {
  auto it = basic_.find(word);
  return it == basic_.end() ? nullptr : &it->second;
}

const Source* KnowledgeBase::source(const std::string& id) const {
  auto it = sources_.find(id);
  return it == sources_.end() ? nullptr : &it->second;
}

const Instruction* KnowledgeBase::instruction(const std::string& mn) const {
  auto it = instructions_.find(upper(mn));
  return it == instructions_.end() ? nullptr : &it->second;
}

const Opcode* KnowledgeBase::opcode(const std::string& mn, const std::string& mode) const {
  const Instruction* in = instruction(mn);
  if (!in) return nullptr;
  for (auto* op : in->opcodes)
    if (op->mode == mode) return op;
  return nullptr;
}

const Register* KnowledgeBase::reg(const std::string& name) const {
  auto it = regIndex_.find(upper(name));
  return it == regIndex_.end() ? nullptr : &registers_[it->second];
}

const CartFormat* KnowledgeBase::format(const std::string& id) const {
  for (auto& f : formats_)
    if (upper(f.id) == upper(id)) return &f;
  return nullptr;
}

const TvStandard* KnowledgeBase::tv(const std::string& id) const {
  for (auto& s : tv_)
    if (upper(s.id) == upper(id)) return &s;
  return nullptr;
}

std::vector<KbEntry> KnowledgeBase::complete(const std::string& prefix, size_t max) const {
  std::vector<KbEntry> out;
  if (prefix.empty()) return out;
  std::string p = upper(prefix);
  auto starts = [&](const std::string& w) { return upper(w).compare(0, p.size(), p) == 0; };
  for (auto& [mn, in] : instructions_)
    if (starts(mn) && out.size() < max) out.push_back({KbEntry::Mnemonic, mn, in.summary});
  for (auto& r : registers_)
    if (starts(r.name) && out.size() < max) out.push_back({KbEntry::Register, r.name, r.summary});
  for (auto& d : dasmDirectives())
    if (starts(d) && out.size() < max) out.push_back({KbEntry::Directive, d, "DASM directive"});
  return out;
}

bool naturalLess(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
      size_t ei = i, ej = j;
      while (ei < a.size() && std::isdigit((unsigned char)a[ei])) ei++;
      while (ej < b.size() && std::isdigit((unsigned char)b[ej])) ej++;
      // compare by length without leading zeros, then digit by digit
      size_t zi = i, zj = j;
      while (zi + 1 < ei && a[zi] == '0') zi++;
      while (zj + 1 < ej && b[zj] == '0') zj++;
      if (ei - zi != ej - zj) return ei - zi < ej - zj;
      int c = a.compare(zi, ei - zi, b, zj, ej - zj);
      if (c != 0) return c < 0;
      i = ei, j = ej;
      continue;
    }
    int ca = std::tolower((unsigned char)a[i]), cb = std::tolower((unsigned char)b[j]);
    if (ca != cb) return ca < cb;
    i++, j++;
  }
  return a.size() - i < b.size() - j;
}

std::vector<std::pair<std::string, std::string>> formatCategories() {
  return {{"atari", T("Atari")},
          {"classic", T("Other makers of the time")},
          {"homebrew", T("Modern bank switching")},
          {"arm", T("ARM cartridges (Harmony, PlusCart, Chameleon...)")}};
}

}  // namespace vcs
