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
#define X(role, id, group, label) {id, group, label},
    VCS_THEME_ROLES(X)
#undef X
};

const RoleInfo& info(Role role) { return kInfo[(int)role]; }

bool roleFromId(const std::string& id, Role* role) {
  for (int i = 0; i < kRoles; i++)
    if (id == kInfo[i].id) {
      if (role) *role = (Role)i;
      return true;
    }
  return false;
}

bool parseColour(const std::string& text, Rgba* out) {
  std::string compact = text;
  compact.erase(std::remove_if(compact.begin(), compact.end(), [](unsigned char character) { return std::isspace(character); }), compact.end());
  if (compact.empty() || compact[0] != '#' || (compact.size() != 7 && compact.size() != 9)) return false;
  for (size_t i = 1; i < compact.size(); i++)
    if (!std::isxdigit((unsigned char)compact[i])) return false;
  Rgba rgba = (Rgba)std::stoul(compact.substr(1), nullptr, 16);
  if (compact.size() == 7) rgba = rgba << 8 | 0xFF;
  if (out) *out = rgba;
  return true;
}

std::string colourText(Rgba colour) {
  char buffer[16];
  if ((colour & 0xFF) == 0xFF) snprintf(buffer, sizeof buffer, "#%06X", (unsigned)(colour >> 8));
  else snprintf(buffer, sizeof buffer, "#%08X", (unsigned)colour);
  return buffer;
}

// ---------------------------------------------------------------------------
// Files

std::string toJson(const Theme& theme) {
  json document;
  document["vcstheme"] = 1;
  document["name"] = theme.name;
  if (!theme.base.empty()) document["base"] = theme.base;
  if (!theme.palette.empty()) document["palette"] = theme.palette;
  // the roles in their order, the rest after
  json colourObject = json::object();
  std::set<std::string> done;
  for (int i = 0; i < kRoles; i++)
    if (auto found = theme.colours.find(kInfo[i].id); found != theme.colours.end()) colourObject[found->first] = found->second, done.insert(found->first);
  for (auto& [name, value] : theme.colours)
    if (!done.count(name)) colourObject[name] = value;
  document["colours"] = colourObject;
  if (theme.hasIndent) document["indent"] = theme.indent;
  if (!theme.imgui.empty()) document["imgui"] = theme.imgui;
  return document.dump(2) + "\n";
}

bool fromJson(const std::string& text, Theme* theme, std::string* error) {
  json document;
  try {
    document = json::parse(text);
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
  if (!document.is_object() || !document.contains("vcstheme")) {
    if (error) *error = T("This is not a VCSIDE theme (it has no \"vcstheme\" key).");
    return false;
  }
  Theme result;
  result.name = document.value("name", "");
  result.base = document.value("base", "");
  auto strings = [&](const char* key, std::map<std::string, std::string>* out) {
    if (!document.contains(key) || !document[key].is_object()) return;
    for (auto& [name, value] : document[key].items())
      if (value.is_string()) (*out)[name] = value.get<std::string>();
  };
  strings("palette", &result.palette);
  strings("colours", &result.colours);
  strings("colors", &result.colours);  // the other spelling works too
  strings("imgui", &result.imgui);
  if (document.contains("indent") && document["indent"].is_array()) {
    result.hasIndent = true;
    for (auto& value : document["indent"])
      if (value.is_string()) result.indent.push_back(value.get<std::string>());
  }
  *theme = result;
  return true;
}

// ---------------------------------------------------------------------------
// Library

static std::string readAll(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

void Library::load(const std::string& builtInDir, const std::string& userDir) {
  themes_.clear();
  userDir_ = userDir;
  for (int pass = 0; pass < 2; pass++) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(pass ? userDir : builtInDir, ec))
      if (entry.path().extension() == ".vcstheme") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    for (auto& file : files) {
      Theme theme;
      std::string error;
      if (!fromJson(readAll(file), &theme, &error)) continue;
      theme.id = file.stem().string();
      theme.path = file.string();
      theme.builtIn = pass == 0;
      if (theme.name.empty()) theme.name = theme.id;
      // a user theme with a built-in one's id replaces it
      auto same = std::find_if(themes_.begin(), themes_.end(), [&](const Theme& existing) { return existing.id == theme.id; });
      if (same != themes_.end()) *same = theme;
      else themes_.push_back(theme);
    }
  }
}

const Theme* Library::find(const std::string& id) const {
  for (auto& theme : themes_)
    if (theme.id == id) return &theme;
  return nullptr;
}
Theme* Library::find(const std::string& id) {
  for (auto& theme : themes_)
    if (theme.id == id) return &theme;
  return nullptr;
}

void Library::merged(const std::string& id, std::map<std::string, std::string>* palette,
                     std::map<std::string, std::string>* colours) const {
  std::vector<const Theme*> chain;
  for (const Theme* theme = find(id); theme && chain.size() < 16; theme = theme->base.empty() ? nullptr : find(theme->base)) {
    if (std::find(chain.begin(), chain.end(), theme) != chain.end()) break;
    chain.push_back(theme);
  }
  for (auto link = chain.rbegin(); link != chain.rend(); ++link) {
    if (palette)
      for (auto& [name, value] : (*link)->palette) (*palette)[name] = value;
    if (colours)
      for (auto& [name, value] : (*link)->colours) (*colours)[name] = value;
  }
}

// "#..", "$name" (palette, which may name another palette entry), "@role", each with an
// optional "/AA" alpha
static bool evalSpec(const std::string& spec, const std::map<std::string, std::string>& palette,
                     const std::map<std::string, std::string>& colours, Rgba* out, int depth) {
  if (depth > 32 || spec.empty()) return false;
  if (size_t slash = spec.rfind('/'); slash != std::string::npos && slash + 3 == spec.size() &&
                                    std::isxdigit((unsigned char)spec[slash + 1]) && std::isxdigit((unsigned char)spec[slash + 2])) {
    Rgba colour;
    if (!evalSpec(spec.substr(0, slash), palette, colours, &colour, depth + 1)) return false;
    *out = (colour & 0xFFFFFF00u) | (Rgba)std::stoul(spec.substr(slash + 1), nullptr, 16);
    return true;
  }
  if (spec[0] == '$') {
    auto paletteEntry = palette.find(spec.substr(1));
    return paletteEntry != palette.end() && evalSpec(paletteEntry->second, palette, colours, out, depth + 1);
  }
  if (spec[0] == '@') {
    auto colourEntry = colours.find(spec.substr(1));
    return colourEntry != colours.end() && evalSpec(colourEntry->second, palette, colours, out, depth + 1);
  }
  return parseColour(spec, out);
}

bool Library::resolveSpec(const std::string& id, const std::string& spec, Rgba* out) const {
  std::map<std::string, std::string> palette, colours;
  merged(id, &palette, &colours);
  return evalSpec(spec, palette, colours, out, 0);
}

Resolved Library::resolve(const std::string& id) const {
  Resolved resolved;
  std::vector<const Theme*> chain;
  for (const Theme* theme = find(id); theme && chain.size() < 16; theme = theme->base.empty() ? nullptr : find(theme->base)) {
    if (std::find(chain.begin(), chain.end(), theme) != chain.end()) {
      resolved.problems.push_back(F("{0}: its bases go round in a loop.", {id}));
      break;
    }
    chain.push_back(theme);
  }
  if (chain.empty()) {
    resolved.problems.push_back(F("There is no theme called {0}.", {id}));
    return resolved;
  }
  if (!chain.back()->base.empty() && !find(chain.back()->base))
    resolved.problems.push_back(F("{0}: there is no base theme called {1}.", {chain.back()->id, chain.back()->base}));
  std::map<std::string, std::string> palette, colours, imgui;
  std::vector<std::string> indent;
  for (auto link = chain.rbegin(); link != chain.rend(); ++link) {
    for (auto& [name, value] : (*link)->palette) palette[name] = value;
    for (auto& [name, value] : (*link)->colours) colours[name] = value;
    for (auto& [name, value] : (*link)->imgui) imgui[name] = value;
    if ((*link)->hasIndent) indent = (*link)->indent;
  }
  auto evaluate = [&](const std::string& spec, Rgba* out, int) { return evalSpec(spec, palette, colours, out, 0); };
  std::vector<int> missing;
  for (int i = 0; i < kRoles; i++) {
    auto found = colours.find(kInfo[i].id);
    if (found == colours.end()) {
      missing.push_back(i);
      resolved.colour[i] = 0xFF00FFFF;  // magenta: easy to spot
    } else if (!evaluate(found->second, &resolved.colour[i], 0)) {
      resolved.problems.push_back(F("{0}: cannot read \"{1}\".", {kInfo[i].id, found->second}));
      resolved.colour[i] = 0xFF00FFFF;
    }
  }
  // A theme of its own with no base (a full copy, say) made before a colour existed:
  // that colour comes from the built-in theme of its kind, dark or light.
  if (!missing.empty() && !chain.back()->builtIn) {
    Rgba background = resolved.colour[(int)Role::UiBackground];
    float lum = (0.299f * (background >> 24) + 0.587f * ((background >> 16) & 255) + 0.114f * ((background >> 8) & 255)) / 255;
    const char* from = lum > 0.5f ? "light" : "dark";
    if (find(from) && find(from)->builtIn) {
      Resolved fallback = resolve(from);
      for (int i : missing) resolved.colour[i] = fallback.colour[i];
      resolved.notes.push_back(F("{0} colours not in this theme come from \"{1}\".", {std::to_string(missing.size()), T(find(from)->name.c_str())}));
      missing.clear();
    }
  }
  for (int i : missing) resolved.problems.push_back(F("{0} has no colour.", {kInfo[i].id}));
  for (auto& [name, value] : colours)
    if (!roleFromId(name, nullptr)) resolved.problems.push_back(F("{0} is not a colour of VCSIDE.", {name}));
  for (auto& spec : indent) {
    Rgba colour;
    if (evaluate(spec, &colour, 0)) resolved.indent.push_back(colour);
    else resolved.problems.push_back(F("indent: cannot read \"{0}\".", {spec}));
  }
  for (auto& [name, spec] : imgui) {
    Rgba colour;
    if (evaluate(spec, &colour, 0)) resolved.imgui[name] = colour;
    else resolved.problems.push_back(F("imgui.{0}: cannot read \"{1}\".", {name, spec}));
  }
  return resolved;
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
    unsigned char byte = (unsigned char)name[i];
    if (byte < 0x80) {
      if (std::isalnum(byte)) base += (char)std::tolower(byte);
      else if (!base.empty() && base.back() != '-') base += '-';
      i++;
      continue;
    }
    size_t length = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : 2;  // one UTF-8 character
    std::string character = name.substr(i, length);
    i += length;
    const char* plain = nullptr;
    for (auto& accent : accents)
      if (character == accent[0]) plain = accent[1];
    if (plain) base += plain;
    else if (!base.empty() && base.back() != '-') base += '-';
  }
  while (!base.empty() && base.back() == '-') base.pop_back();
  if (base.empty()) base = "theme";
  std::string id = base;
  for (int suffix = 2; find(id); suffix++) id = base + "-" + std::to_string(suffix);
  return id;
}

bool Library::save(Theme& theme, std::string* error) {
  if (theme.id.empty()) theme.id = uniqueId(theme.name);
  std::error_code ec;
  fs::create_directories(userDir_, ec);
  fs::path filePath = fs::path(userDir_) / (theme.id + ".vcstheme");
  std::ofstream file(filePath, std::ios::binary);
  if (!file) {
    if (error) *error = F("Cannot write {0}", {filePath.string()});
    return false;
  }
  file << toJson(theme);
  theme.path = filePath.string();
  theme.builtIn = false;
  if (Theme* existing = find(theme.id)) *existing = theme;
  else themes_.push_back(theme);
  return true;
}

bool Library::remove(const std::string& id, std::string* error) {
  auto found = std::find_if(themes_.begin(), themes_.end(), [&](const Theme& theme) { return theme.id == id; });
  if (found == themes_.end() || found->builtIn) {
    if (error) *error = T("Built-in themes cannot be deleted.");
    return false;
  }
  std::error_code ec;
  fs::remove(found->path, ec);
  themes_.erase(found);
  return true;
}

bool Library::importText(const std::string& text, const std::string& fallbackName, std::string* id, std::string* error) {
  Theme theme;
  if (!fromJson(text, &theme, error)) return false;
  if (theme.name.empty()) theme.name = fallbackName;
  theme.id = uniqueId(fallbackName.empty() ? theme.name : fallbackName);
  if (!save(theme, error)) return false;
  if (id) *id = theme.id;
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
  const Theme* theme = find(id);
  if (!theme) return "";
  Theme out = *theme;
  // up the chain until a built-in theme: what is in between goes into this one
  std::vector<const Theme*> chain;
  const Theme* base = theme->base.empty() ? nullptr : find(theme->base);
  while (base && !base->builtIn && chain.size() < 16 && std::find(chain.begin(), chain.end(), base) == chain.end()) {
    chain.push_back(base);
    base = base->base.empty() ? nullptr : find(base->base);
  }
  out.base = base ? base->id : "";
  for (auto* ancestor : chain) {  // nearest first: what the theme says itself wins
    for (auto& [name, value] : ancestor->palette) out.palette.emplace(name, value);
    for (auto& [name, value] : ancestor->colours) out.colours.emplace(name, value);
    for (auto& [name, value] : ancestor->imgui) out.imgui.emplace(name, value);
    if (!out.hasIndent && ancestor->hasIndent) out.indent = ancestor->indent, out.hasIndent = true;
  }
  return toJson(out);
}

// ---------------------------------------------------------------------------

static Resolved& cur() {
  static Resolved resolved;
  return resolved;
}
void setCurrent(const Resolved& resolved) { cur() = resolved; }
const Resolved& current() { return cur(); }
Rgba colour(Role role) { return cur().colour[(int)role]; }

}  // namespace vcs::theme
