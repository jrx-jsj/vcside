// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "folder_dialog.h"

#ifndef __APPLE__  // macOS: src/platform/folder_dialog_macos.mm
namespace vcs {
void showFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window, const char* defaultLocation) {
  SDL_ShowOpenFolderDialog(callback, userdata, window, defaultLocation, false);
}

void showProjectDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window) {
  static const SDL_DialogFileFilter filters[] = {{"VCSIDE project", "vcsproj"}};
  SDL_ShowOpenFileDialog(callback, userdata, window, filters, 1, nullptr, false);
}
}  // namespace vcs
#endif
