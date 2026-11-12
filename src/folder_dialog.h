// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Choosing a folder. On macOS the panel can also make a new folder (SDL's does not offer
// it there); elsewhere it is SDL's, whose system dialogs already can.
#pragma once

#include <SDL3/SDL.h>

namespace vcs {
// As SDL_ShowOpenFolderDialog: callback gets the chosen folder (or none), once.
void showFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window, const char* defaultLocation);
}  // namespace vcs
