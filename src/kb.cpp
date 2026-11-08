// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "kb.h"

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
  return ok && (!error || error->empty());
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

}  // namespace vcs
