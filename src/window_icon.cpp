// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "window_icon.h"

#include <string>

#include "paths.h"

// SDL 3.4 reads PNG itself; older SDL 3 builds use stb_image (MIT / public domain).
#if !SDL_VERSION_ATLEAST(3, 4, 0)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_WINDOWS_UTF8
#include "stb_image.h"
#endif

namespace vcs {

SDL_Surface* loadIconSurface() {
  const std::string path = dataDir() + "/vcside.png";
#if SDL_VERSION_ATLEAST(3, 4, 0)
  SDL_Surface* icon = SDL_LoadPNG(path.c_str());
#else
  SDL_Surface* icon = nullptr;
  int width = 0, height = 0, channels = 0;
  if (unsigned char* pixelData = stbi_load(path.c_str(), &width, &height, &channels, 4)) {
    if (SDL_Surface* view = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixelData, width * 4)) {
      icon = SDL_DuplicateSurface(view);
      SDL_DestroySurface(view);
    }
    stbi_image_free(pixelData);
  }
#endif
  if (!icon) {
    SDL_Log("Icon %s: %s", path.c_str(), SDL_GetError());
    return nullptr;
  }
  if (icon->format != SDL_PIXELFORMAT_RGBA32) {
    SDL_Surface* rgba = SDL_ConvertSurface(icon, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(icon);
    icon = rgba;
  }
  return icon;
}

void setWindowIcon(SDL_Window* window) {
  if (SDL_Surface* icon = loadIconSurface()) {
    SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
  }
}

}  // namespace vcs
