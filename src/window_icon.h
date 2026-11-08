// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// Window / taskbar icon, loaded from data/vcside.png.
#pragma once
#include <SDL3/SDL.h>

namespace vcs {

void setWindowIcon(SDL_Window* window);
// The icon (data/vcside.png) as RGBA32 pixels, for the About window. Null if missing.
SDL_Surface* loadIconSurface();

}  // namespace vcs
