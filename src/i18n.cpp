// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
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

void loadLanguages(const std::string& directory) {
  catalogs.clear();
  std::error_code errorCode;
  for (auto& entry : fs::directory_iterator(directory, errorCode)) {
    if (entry.path().extension() != ".json") continue;
    try {
      std::ifstream file(entry.path());
      nlohmann::json catalogJson = nlohmann::json::parse(file);
      Catalog catalog;
      catalog.info.code = catalogJson.value("language", entry.path().stem().string());
      catalog.info.name = catalogJson.value("name", catalog.info.code);
      if (catalog.info.code == "en") continue;  // English is the source language
      nlohmann::json strings = catalogJson.value("strings", nlohmann::json::object());
      for (auto& [key, value] : strings.items())
        if (value.is_string() && !value.get<std::string>().empty()) catalog.strings[key] = value.get<std::string>();
      catalog.info.translated = (int)catalog.strings.size();
      catalog.info.total = catalogJson.value("total", 0);
      catalogs.push_back(std::move(catalog));
    } catch (const std::exception& exception) {
      SDL_Log("i18n: %s: %s", entry.path().string().c_str(), exception.what());
    }
  }
  std::sort(catalogs.begin(), catalogs.end(), [](auto& left, auto& right) { return left.info.code < right.info.code; });
  infos.clear();
  infos.push_back({"en", "English", 0, 0});
  for (auto& catalog : catalogs) infos.push_back(catalog.info);
  setLanguage(current);
}

const std::vector<LanguageInfo>& languages() { return infos; }

void setLanguage(const std::string& code) {
  std::string normalized = code;
  std::replace(normalized.begin(), normalized.end(), '_', '-');
  active = nullptr;
  current = "en";
  for (const std::string& want : {normalized, normalized.substr(0, normalized.find('-'))})
    for (auto& catalog : catalogs)
      if (SDL_strcasecmp(catalog.info.code.c_str(), want.c_str()) == 0) {
        active = &catalog;
        current = catalog.info.code;
        return;
      }
}

const std::string& currentLanguage() { return current; }

std::string systemLanguage() {
  int localeCount = 0;
  SDL_Locale** locales = SDL_GetPreferredLocales(&localeCount);
  std::string best = "en";
  for (int i = 0; locales && i < localeCount; i++) {
    std::string code = locales[i]->language ? locales[i]->language : "";
    if (locales[i]->country) code += std::string("-") + locales[i]->country;
    std::string previous = current;
    setLanguage(code);
    bool found = current != "en" || code.rfind("en", 0) == 0;
    std::string chosen = current;
    setLanguage(previous);
    if (found) {
      best = chosen;
      break;
    }
  }
  SDL_free(locales);
  return best;
}

const char* T(const char* english) {
  if (!active) return english;
  auto found = active->strings.find(english);
  return found == active->strings.end() ? english : found->second.c_str();
}

std::string F(const char* english, std::initializer_list<std::string> args) {
  std::string text = T(english), result;
  std::vector<std::string> arguments(args);
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '{') {
      size_t close = text.find('}', i);
      if (close != std::string::npos && close > i + 1 && close - i <= 3) {
        bool digits = true;
        for (size_t digitIndex = i + 1; digitIndex < close; digitIndex++) digits &= std::isdigit((unsigned char)text[digitIndex]) != 0;
        if (digits) {
          size_t index = std::stoul(text.substr(i + 1, close - i - 1));
          if (index < arguments.size()) result += arguments[index];
          i = close;
          continue;
        }
      }
    }
    result += text[i];
  }
  return result;
}

}  // namespace vcs
