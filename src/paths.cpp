// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "paths.h"

#include <SDL3/SDL.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace vcs {

std::string dataDir() {
  static std::string cached;
  if (!cached.empty()) return cached;
  std::string base = SDL_GetBasePath() ? SDL_GetBasePath() : "";
  for (const char* rel : {"data", "../Resources/data", "../data", "../../data"}) {
    fs::path p = fs::path(base) / rel;
    std::error_code ec;
    if (fs::is_directory(p / "kb", ec)) return cached = fs::weakly_canonical(p).string();
  }
  return cached = (fs::path(base) / "data").string();
}

std::string configDir() {
  // VCSIDE_CONFIG_DIR: another settings folder (tests that must not touch the real one)
  if (const char* env = SDL_getenv("VCSIDE_CONFIG_DIR"); env && *env) {
    std::error_code ec;
    fs::create_directories(env, ec);
    std::string s = env;
    return s.back() == '/' || s.back() == '\\' ? s : s + "/";
  }
  char* p = SDL_GetPrefPath("vcside", "VCSIDE");
  std::string s = p ? p : "";
  SDL_free(p);
  return s;
}

}  // namespace vcs
