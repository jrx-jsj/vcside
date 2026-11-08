// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// The menu bar as data, drawn by ImGui (Windows, Linux) or by the system (macOS),
// where it goes in the screen's menu bar like in every other app.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace vcs {

struct MenuItem {
  std::string label;
  std::string shortcut;  // "Ctrl+Shift+S", "F7", "Alt+Up"; Ctrl is Cmd on macOS. Only shown:
                         // the keys themselves are handled by the app, not by the menu.
  std::string hint;      // extra text (ImGui shows it on the right; macOS appends it)
  bool enabled = true, checked = false, separator = false;
  std::function<void()> action;
  std::vector<MenuItem> children;  // not empty: a submenu

  static MenuItem sep() {
    MenuItem m;
    m.separator = true;
    return m;
  }
};

// Top-level menus. On macOS the first one is the application menu's own part
// (About, Settings...); the system adds Services, Hide and Quit around it.
using MenuBar = std::vector<MenuItem>;

// Draws the menus inside an ImGui menu bar (between BeginMainMenuBar/EndMainMenuBar).
void drawImGuiMenus(const MenuBar& bar);

// System menu bar. available() is false where there is none (anything but macOS).
struct NativeMenuCallbacks {
  std::function<MenuBar()> build;  // called whenever a menu opens, so it is always current
  std::function<void()> quit;      // "Quit VCSIDE" in the application menu
  std::function<void()> about;     // "About VCSIDE"
};
bool nativeMenuAvailable();
void nativeMenuInstall(const NativeMenuCallbacks& cb);
void nativeMenuRefresh();  // after a language change (top-level titles)
// Actions chosen in the system menu, to run at the start of the next frame.
std::vector<std::function<void()>> nativeMenuTakeActions();
// Before SDL starts (macOS): no window restoration, so a crash never brings
// back the "reopen windows?" alert, which would block the launch.
void nativeEarlyInit();
// Text dump of the system menu (for tests); opens every menu's update first.
std::string nativeMenuDump();
// Chooses an item by path ("File/Save"), as if clicked (for tests).
bool nativeMenuPerform(const std::string& path);

}  // namespace vcs
