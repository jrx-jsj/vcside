// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "i18n.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace fs = std::filesystem;

namespace vcs {

namespace {
struct Catalog {
  LanguageInfo info;
  std::unordered_map<std::string, std::string> strings;
};
std::vector<Catalog> catalogs;
std::vector<LanguageInfo> infos;
std::string current = "en";
const Catalog* active = nullptr;
}  // namespace

void loadLanguages(const std::string& dir) {
  catalogs.clear();
  std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".json") continue;
    try {
      std::ifstream f(e.path());
      nlohmann::json j = nlohmann::json::parse(f);
      Catalog c;
      c.info.code = j.value("language", e.path().stem().string());
      c.info.name = j.value("name", c.info.code);
      if (c.info.code == "en") continue;  // English is the source language
      nlohmann::json strings = j.value("strings", nlohmann::json::object());
      for (auto& [k, v] : strings.items())
        if (v.is_string() && !v.get<std::string>().empty()) c.strings[k] = v.get<std::string>();
      c.info.translated = (int)c.strings.size();
      c.info.total = j.value("total", 0);
      catalogs.push_back(std::move(c));
    } catch (const std::exception& ex) {
      SDL_Log("i18n: %s: %s", e.path().string().c_str(), ex.what());
    }
  }
  std::sort(catalogs.begin(), catalogs.end(), [](auto& a, auto& b) { return a.info.code < b.info.code; });
  infos.clear();
  infos.push_back({"en", "English", 0, 0});
  for (auto& c : catalogs) infos.push_back(c.info);
  setLanguage(current);
}

const std::vector<LanguageInfo>& languages() { return infos; }

void setLanguage(const std::string& code) {
  std::string norm = code;
  std::replace(norm.begin(), norm.end(), '_', '-');
  active = nullptr;
  current = "en";
  for (const std::string& want : {norm, norm.substr(0, norm.find('-'))})
    for (auto& c : catalogs)
      if (SDL_strcasecmp(c.info.code.c_str(), want.c_str()) == 0) {
        active = &c;
        current = c.info.code;
        return;
      }
}

const std::string& currentLanguage() { return current; }

std::string systemLanguage() {
  int n = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&n);
  std::string best = "en";
  for (int i = 0; locales && i < n; i++) {
    std::string code = locales[i]->language ? locales[i]->language : "";
    if (locales[i]->country) code += std::string("-") + locales[i]->country;
    std::string prev = current;
    setLanguage(code);
    bool found = current != "en" || code.rfind("en", 0) == 0;
    std::string got = current;
    setLanguage(prev);
    if (found) {
      best = got;
      break;
    }
  }
  SDL_free(locales);
  return best;
}

const char* T(const char* en) {
  if (!active) return en;
  auto it = active->strings.find(en);
  return it == active->strings.end() ? en : it->second.c_str();
}

std::string F(const char* en, std::initializer_list<std::string> args) {
  std::string t = T(en), out;
  std::vector<std::string> a(args);
  for (size_t i = 0; i < t.size(); i++) {
    if (t[i] == '{') {
      size_t close = t.find('}', i);
      if (close != std::string::npos && close > i + 1 && close - i <= 3) {
        bool digits = true;
        for (size_t k = i + 1; k < close; k++) digits &= std::isdigit((unsigned char)t[k]) != 0;
        if (digits) {
          size_t n = std::stoul(t.substr(i + 1, close - i - 1));
          if (n < a.size()) out += a[n];
          i = close;
          continue;
        }
      }
    }
    out += t[i];
  }
  return out;
}

}  // namespace vcs
