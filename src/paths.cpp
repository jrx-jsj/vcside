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
  for (const char* relativePath : {"data", "../Resources/data", "../data", "../../data"}) {
    fs::path candidate = fs::path(base) / relativePath;
    std::error_code errorCode;
    if (fs::is_directory(candidate / "kb", errorCode)) return cached = fs::weakly_canonical(candidate).string();
  }
  return cached = (fs::path(base) / "data").string();
}

std::string configDir() {
  // VCSIDE_CONFIG_DIR: another settings folder (tests that must not touch the real one)
  if (const char* configOverride = SDL_getenv("VCSIDE_CONFIG_DIR"); configOverride && *configOverride) {
    std::error_code errorCode;
    fs::create_directories(configOverride, errorCode);
    std::string folder = configOverride;
    return folder.back() == '/' || folder.back() == '\\' ? folder : folder + "/";
  }
  char* prefPath = SDL_GetPrefPath("vcside", "VCSIDE");
  std::string folder = prefPath ? prefPath : "";
  SDL_free(prefPath);
  return folder;
}

}  // namespace vcs
