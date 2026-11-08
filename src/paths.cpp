// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
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
  char* p = SDL_GetPrefPath("vcside", "VCSIDE");
  std::string s = p ? p : "";
  SDL_free(p);
  return s;
}

}  // namespace vcs
