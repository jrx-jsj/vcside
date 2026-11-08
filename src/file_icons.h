// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The kinds of files in a 2600 project and the small icons the project tree draws for
// them (vector shapes, so they follow the theme and any zoom).
#pragma once
#include <string>

struct ImDrawList;
struct ImVec2;

namespace vcs::files {

enum class Kind { Folder, Project, Asm, C, Header, Basic, Graphics, Rom, Image, Listing, Text };

// By name: "main.asm" -> Asm, "hero.vcsgfx" -> Graphics, "Makefile" -> Text...
Kind kindOf(const std::string& filename, bool directory);

// The icon in a square of side `size` at `pos`; `open` is for folders.
void drawIcon(ImDrawList* dl, ImVec2 pos, float size, Kind kind, bool open = false);

}  // namespace vcs::files
