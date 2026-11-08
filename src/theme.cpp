// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "theme.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

#include "i18n.h"

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace vcs::theme {

static const RoleInfo kInfo[] = {
#define X(e, id, group, label) {id, group, label},
    VCS_THEME_ROLES(X)
#undef X
};

const RoleInfo& info(Role r) { return kInfo[(int)r]; }

bool roleFromId(const std::string& id, Role* r) {
  for (int i = 0; i < kRoles; i++)
    if (id == kInfo[i].id) {
      if (r) *r = (Role)i;
      return true;
    }
  return false;
}

bool parseColour(const std::string& text, Rgba* out) {
  std::string t = text;
  t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c) { return std::isspace(c); }), t.end());
  if (t.empty() || t[0] != '#' || (t.size() != 7 && t.size() != 9)) return false;
  for (size_t i = 1; i < t.size(); i++)
    if (!std::isxdigit((unsigned char)t[i])) return false;
  Rgba v = (Rgba)std::stoul(t.substr(1), nullptr, 16);
  if (t.size() == 7) v = v << 8 | 0xFF;
  if (out) *out = v;
  return true;
}

std::string colourText(Rgba c) {
  char b[16];
  if ((c & 0xFF) == 0xFF) snprintf(b, sizeof b, "#%06X", (unsigned)(c >> 8));
  else snprintf(b, sizeof b, "#%08X", (unsigned)c);
  return b;
}

// ---------------------------------------------------------------------------
// Files

std::string toJson(const Theme& t) {
  json j;
  j["vcstheme"] = 1;
  j["name"] = t.name;
  if (!t.base.empty()) j["base"] = t.base;
  if (!t.palette.empty()) j["palette"] = t.palette;
  // the roles in their order, the rest after
  json c = json::object();
  std::set<std::string> done;
  for (int i = 0; i < kRoles; i++)
    if (auto it = t.colours.find(kInfo[i].id); it != t.colours.end()) c[it->first] = it->second, done.insert(it->first);
  for (auto& [k, v] : t.colours)
    if (!done.count(k)) c[k] = v;
  j["colours"] = c;
  if (t.hasIndent) j["indent"] = t.indent;
  if (!t.imgui.empty()) j["imgui"] = t.imgui;
  return j.dump(2) + "\n";
}

bool fromJson(const std::string& text, Theme* t, std::string* error) {
  json j;
  try {
    j = json::parse(text);
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
  if (!j.is_object() || !j.contains("vcstheme")) {
    if (error) *error = T("This is not a VCSIDE theme (it has no \"vcstheme\" key).");
    return false;
  }
  Theme r;
  r.name = j.value("name", "");
  r.base = j.value("base", "");
  auto strings = [&](const char* key, std::map<std::string, std::string>* out) {
    if (!j.contains(key) || !j[key].is_object()) return;
    for (auto& [k, v] : j[key].items())
      if (v.is_string()) (*out)[k] = v.get<std::string>();
  };
  strings("palette", &r.palette);
  strings("colours", &r.colours);
  strings("colors", &r.colours);  // the other spelling works too
  strings("imgui", &r.imgui);
  if (j.contains("indent") && j["indent"].is_array()) {
    r.hasIndent = true;
    for (auto& v : j["indent"])
      if (v.is_string()) r.indent.push_back(v.get<std::string>());
  }
  *t = r;
  return true;
}

// ---------------------------------------------------------------------------
// Library

static std::string readAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

void Library::load(const std::string& builtInDir, const std::string& userDir) {
  themes_.clear();
  userDir_ = userDir;
  for (int pass = 0; pass < 2; pass++) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(pass ? userDir : builtInDir, ec))
      if (e.path().extension() == ".vcstheme") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
      Theme t;
      std::string err;
      if (!fromJson(readAll(f), &t, &err)) continue;
      t.id = f.stem().string();
      t.path = f.string();
      t.builtIn = pass == 0;
      if (t.name.empty()) t.name = t.id;
      // a user theme with a built-in one's id replaces it
      auto same = std::find_if(themes_.begin(), themes_.end(), [&](const Theme& o) { return o.id == t.id; });
      if (same != themes_.end()) *same = t;
      else themes_.push_back(t);
    }
  }
}

const Theme* Library::find(const std::string& id) const {
  for (auto& t : themes_)
    if (t.id == id) return &t;
  return nullptr;
}
Theme* Library::find(const std::string& id) {
  for (auto& t : themes_)
    if (t.id == id) return &t;
  return nullptr;
}

void Library::merged(const std::string& id, std::map<std::string, std::string>* palette,
                     std::map<std::string, std::string>* colours) const {
  std::vector<const Theme*> chain;
  for (const Theme* t = find(id); t && chain.size() < 16; t = t->base.empty() ? nullptr : find(t->base)) {
    if (std::find(chain.begin(), chain.end(), t) != chain.end()) break;
    chain.push_back(t);
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    if (palette)
      for (auto& [k, v] : (*it)->palette) (*palette)[k] = v;
    if (colours)
      for (auto& [k, v] : (*it)->colours) (*colours)[k] = v;
  }
}

// "#..", "$name" (palette, which may name another palette entry), "@role", each with an
// optional "/AA" alpha
static bool evalSpec(const std::string& v, const std::map<std::string, std::string>& palette,
                     const std::map<std::string, std::string>& colours, Rgba* out, int depth) {
  if (depth > 32 || v.empty()) return false;
  if (size_t slash = v.rfind('/'); slash != std::string::npos && slash + 3 == v.size() &&
                                    std::isxdigit((unsigned char)v[slash + 1]) && std::isxdigit((unsigned char)v[slash + 2])) {
    Rgba c;
    if (!evalSpec(v.substr(0, slash), palette, colours, &c, depth + 1)) return false;
    *out = (c & 0xFFFFFF00u) | (Rgba)std::stoul(v.substr(slash + 1), nullptr, 16);
    return true;
  }
  if (v[0] == '$') {
    auto p = palette.find(v.substr(1));
    return p != palette.end() && evalSpec(p->second, palette, colours, out, depth + 1);
  }
  if (v[0] == '@') {
    auto c = colours.find(v.substr(1));
    return c != colours.end() && evalSpec(c->second, palette, colours, out, depth + 1);
  }
  return parseColour(v, out);
}

bool Library::resolveSpec(const std::string& id, const std::string& spec, Rgba* out) const {
  std::map<std::string, std::string> palette, colours;
  merged(id, &palette, &colours);
  return evalSpec(spec, palette, colours, out, 0);
}

Resolved Library::resolve(const std::string& id) const {
  Resolved r;
  std::vector<const Theme*> chain;
  for (const Theme* t = find(id); t && chain.size() < 16; t = t->base.empty() ? nullptr : find(t->base)) {
    if (std::find(chain.begin(), chain.end(), t) != chain.end()) {
      r.problems.push_back(F("{0}: its bases go round in a loop.", {id}));
      break;
    }
    chain.push_back(t);
  }
  if (chain.empty()) {
    r.problems.push_back(F("There is no theme called {0}.", {id}));
    return r;
  }
  if (!chain.back()->base.empty() && !find(chain.back()->base))
    r.problems.push_back(F("{0}: there is no base theme called {1}.", {chain.back()->id, chain.back()->base}));
  std::map<std::string, std::string> palette, colours, imgui;
  std::vector<std::string> indent;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    for (auto& [k, v] : (*it)->palette) palette[k] = v;
    for (auto& [k, v] : (*it)->colours) colours[k] = v;
    for (auto& [k, v] : (*it)->imgui) imgui[k] = v;
    if ((*it)->hasIndent) indent = (*it)->indent;
  }
  auto value = [&](const std::string& v, Rgba* out, int) { return evalSpec(v, palette, colours, out, 0); };
  for (int i = 0; i < kRoles; i++) {
    auto c = colours.find(kInfo[i].id);
    if (c == colours.end()) {
      r.problems.push_back(F("{0} has no colour.", {kInfo[i].id}));
      r.colour[i] = 0xFF00FFFF;  // magenta: easy to spot
    } else if (!value(c->second, &r.colour[i], 0)) {
      r.problems.push_back(F("{0}: cannot read \"{1}\".", {kInfo[i].id, c->second}));
      r.colour[i] = 0xFF00FFFF;
    }
  }
  for (auto& [k, v] : colours)
    if (!roleFromId(k, nullptr)) r.problems.push_back(F("{0} is not a colour of VCSIDE.", {k}));
  for (auto& v : indent) {
    Rgba c;
    if (value(v, &c, 0)) r.indent.push_back(c);
    else r.problems.push_back(F("indent: cannot read \"{0}\".", {v}));
  }
  for (auto& [k, v] : imgui) {
    Rgba c;
    if (value(v, &c, 0)) r.imgui[k] = c;
    else r.problems.push_back(F("imgui.{0}: cannot read \"{1}\".", {k, v}));
  }
  return r;
}

std::string Library::uniqueId(const std::string& name) const {
  // Latin letters with accents become their plain letter: the id is a file name
  static const char* accents[][2] = {{"á", "a"}, {"à", "a"}, {"â", "a"}, {"ä", "a"}, {"ã", "a"}, {"å", "a"}, {"é", "e"},
                                     {"è", "e"}, {"ê", "e"}, {"ë", "e"}, {"í", "i"}, {"ì", "i"}, {"î", "i"}, {"ï", "i"},
                                     {"ó", "o"}, {"ò", "o"}, {"ô", "o"}, {"ö", "o"}, {"õ", "o"}, {"ú", "u"}, {"ù", "u"},
                                     {"û", "u"}, {"ü", "u"}, {"ñ", "n"}, {"ç", "c"}, {"Á", "a"}, {"É", "e"}, {"Í", "i"},
                                     {"Ó", "o"}, {"Ú", "u"}, {"Ü", "u"}, {"Ñ", "n"}, {"Ç", "c"}};
  std::string base;
  for (size_t i = 0; i < name.size();) {
    unsigned char c = (unsigned char)name[i];
    if (c < 0x80) {
      if (std::isalnum(c)) base += (char)std::tolower(c);
      else if (!base.empty() && base.back() != '-') base += '-';
      i++;
      continue;
    }
    size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;  // one UTF-8 character
    std::string ch = name.substr(i, n);
    i += n;
    const char* plain = nullptr;
    for (auto& a : accents)
      if (ch == a[0]) plain = a[1];
    if (plain) base += plain;
    else if (!base.empty() && base.back() != '-') base += '-';
  }
  while (!base.empty() && base.back() == '-') base.pop_back();
  if (base.empty()) base = "theme";
  std::string id = base;
  for (int n = 2; find(id); n++) id = base + "-" + std::to_string(n);
  return id;
}

bool Library::save(Theme& t, std::string* error) {
  if (t.id.empty()) t.id = uniqueId(t.name);
  std::error_code ec;
  fs::create_directories(userDir_, ec);
  fs::path p = fs::path(userDir_) / (t.id + ".vcstheme");
  std::ofstream f(p, std::ios::binary);
  if (!f) {
    if (error) *error = F("Cannot write {0}", {p.string()});
    return false;
  }
  f << toJson(t);
  t.path = p.string();
  t.builtIn = false;
  if (Theme* old = find(t.id)) *old = t;
  else themes_.push_back(t);
  return true;
}

bool Library::remove(const std::string& id, std::string* error) {
  auto it = std::find_if(themes_.begin(), themes_.end(), [&](const Theme& t) { return t.id == id; });
  if (it == themes_.end() || it->builtIn) {
    if (error) *error = T("Built-in themes cannot be deleted.");
    return false;
  }
  std::error_code ec;
  fs::remove(it->path, ec);
  themes_.erase(it);
  return true;
}

bool Library::importText(const std::string& text, const std::string& fallbackName, std::string* id, std::string* error) {
  Theme t;
  if (!fromJson(text, &t, error)) return false;
  if (t.name.empty()) t.name = fallbackName;
  t.id = uniqueId(fallbackName.empty() ? t.name : fallbackName);
  if (!save(t, error)) return false;
  if (id) *id = t.id;
  return true;
}

bool Library::import(const std::string& path, std::string* id, std::string* error) {
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (error) *error = F("Cannot read {0}", {path});
    return false;
  }
  return importText(readAll(path), fs::path(path).stem().string(), id, error);
}

std::string Library::exportJson(const std::string& id) const {
  const Theme* t = find(id);
  if (!t) return "";
  Theme out = *t;
  // up the chain until a built-in theme: what is in between goes into this one
  std::vector<const Theme*> chain;
  const Theme* b = t->base.empty() ? nullptr : find(t->base);
  while (b && !b->builtIn && chain.size() < 16 && std::find(chain.begin(), chain.end(), b) == chain.end()) {
    chain.push_back(b);
    b = b->base.empty() ? nullptr : find(b->base);
  }
  out.base = b ? b->id : "";
  for (auto* c : chain) {  // nearest first: what the theme says itself wins
    for (auto& [k, v] : c->palette) out.palette.emplace(k, v);
    for (auto& [k, v] : c->colours) out.colours.emplace(k, v);
    for (auto& [k, v] : c->imgui) out.imgui.emplace(k, v);
    if (!out.hasIndent && c->hasIndent) out.indent = c->indent, out.hasIndent = true;
  }
  return toJson(out);
}

// ---------------------------------------------------------------------------

static Resolved& cur() {
  static Resolved r;
  return r;
}
void setCurrent(const Resolved& r) { cur() = r; }
const Resolved& current() { return cur(); }
Rgba colour(Role r) { return cur().colour[(int)r]; }

}  // namespace vcs::theme
