// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
#include "menu.h"

#include <imgui.h>

namespace vcs {

static void drawItems(const std::vector<MenuItem>& items) {
  for (auto& m : items) {
    if (m.separator) {
      ImGui::Separator();
    } else if (!m.children.empty()) {
      if (ImGui::BeginMenu(m.label.c_str(), m.enabled)) {
        drawItems(m.children);
        ImGui::EndMenu();
      }
    } else {
      const std::string& right = m.shortcut.empty() ? m.hint : m.shortcut;
      if (ImGui::MenuItem(m.label.c_str(), right.empty() ? nullptr : right.c_str(), m.checked, m.enabled) && m.action)
        m.action();
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
