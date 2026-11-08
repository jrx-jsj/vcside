// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Where VCSIDE finds its data (knowledge base, templates) and keeps its settings.
#pragma once
#include <string>

namespace vcs {

// data/ next to the executable (Contents/Resources/data inside the macOS bundle).
std::string dataDir();
// Per-user settings folder (SDL_GetPrefPath).
std::string configDir();

}  // namespace vcs
