// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Interface translations.
//
// Catalogs live in data/i18n/<code>.json:
//   {"language": "es", "name": "Español", "strings": {"File": "Archivo", ...}}
// Keys are the English source strings (gettext style). A catalog may be partial:
// any string it lacks is shown in English. Adding a language = adding a file.
// tools/i18n_check.py lists the strings missing or unused in each catalog.
#pragma once
#include <string>
#include <vector>

namespace vcs {

struct LanguageInfo {
  std::string code;  // "en", "es", "pt-BR"...
  std::string name;  // in its own language
  int translated = 0, total = 0;
};

// Scans dir for catalogs. English is always available (it needs no file).
void loadLanguages(const std::string& directory);
const std::vector<LanguageInfo>& languages();
// Selects a language by code; unknown codes fall back to English. Also accepts
// a locale like "es_ES" (tries "es_ES", then "es").
void setLanguage(const std::string& code);
const std::string& currentLanguage();
// Best match for the system locale (SDL_GetPreferredLocales), or "en".
std::string systemLanguage();

// Translates an English UI string; returns it unchanged if the current
// catalog has no translation. The returned pointer stays valid.
const char* T(const char* english);
// Marks a string for translation without translating it (for tables that are
// translated later with T()). tools/i18n_check.py collects T, F and N_.
#define N_(s) s
inline std::string Ts(const std::string& english) { return T(english.c_str()); }
// Translates a template with numbered placeholders ({0}, {1}...) and fills them,
// so a translation can reorder them (e.g. "{0} lines: {1} cycles" -> "{1} ciclos en {0} líneas").
std::string F(const char* english, std::initializer_list<std::string> arguments);

}  // namespace vcs
