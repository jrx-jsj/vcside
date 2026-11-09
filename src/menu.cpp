// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "menu.h"

#include <imgui.h>

namespace vcs {

static void drawItems(const std::vector<MenuItem>& items) {
  for (auto& item : items) {
    if (item.separator) {
      ImGui::Separator();
    } else if (!item.children.empty()) {
      if (ImGui::BeginMenu(item.label.c_str(), item.enabled)) {
        drawItems(item.children);
        ImGui::EndMenu();
      }
    } else {
      const std::string& right = item.shortcut.empty() ? item.hint : item.shortcut;
      if (ImGui::MenuItem(item.label.c_str(), right.empty() ? nullptr : right.c_str(), item.checked, item.enabled) && item.action)
        item.action();
    }
  }
}

void drawImGuiMenus(const MenuBar& bar) { drawItems(bar); }

#ifndef __APPLE__
bool nativeMenuAvailable() { return false; }
void nativeEarlyInit() {}
void nativeMenuInstall(const NativeMenuCallbacks&) {}
void nativeMenuRefresh() {}
std::vector<std::function<void()>> nativeMenuTakeActions() { return {}; }
std::string nativeMenuDump() { return {}; }
bool nativeMenuPerform(const std::string&) { return false; }
#endif

}  // namespace vcs
