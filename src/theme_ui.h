// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Themes in ImGui: the current colours as ImU32/ImVec4, the ImGui style that follows
// from a theme, and the window that edits themes.
#pragma once
#include <functional>
#include <string>

#include "imgui.h"
#include "theme.h"

namespace vcs::theme {

// A size in pixels at 100 % zoom, scaled like the current font (the IDE's zoom).
inline float em(float px) { return px * ImGui::GetFontSize() / 15.0f; }

inline ImU32 u32(Rgba c) { return IM_COL32(c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255); }
inline ImVec4 vec(Rgba c) { return ImVec4((c >> 24) / 255.0f, ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f); }
inline ImU32 col(Role r) { return u32(colour(r)); }
inline ImVec4 colv(Role r) { return vec(colour(r)); }

// The name to show: built-in themes are translated.
std::string displayName(const Theme& t);

// Sets every ImGui colour from the roles, then the theme's own "imgui" ones.
void applyImGui(const Resolved& r);

class Editor {
 public:
  struct Hooks {
    std::function<void(const std::string& id)> use;     // make it the current theme
    std::function<void()> importFile;                   // File → open dialog
    std::function<void(const std::string& id)> exportFile;
    std::function<void(const std::string& dir)> openFolder;
  };
  bool open = false;
  // `current` is the theme in use; editing a theme makes it the current one.
  void draw(Library& lib, const std::string& current, const Hooks& h);

 private:
  std::string selected_, message_, filter_;
  bool dirty_ = false;
  char newPalette_[64] = "";
  char newName_[128] = "";
  bool duplicate_ = false, focusName_ = false;
};

}  // namespace vcs::theme
