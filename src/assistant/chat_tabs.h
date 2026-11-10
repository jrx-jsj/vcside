// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The tab bar of the assistant's open conversations. Only the drawing and its protocol live
// here (ImGui's tab bar selects a tab by itself when it is clicked, and a tab the program
// wants selected must be asked for in the frame after it was added), so the protocol can be
// tested without the panel: tests/tests.cpp runs it on a headless ImGui.
#pragma once
#include <imgui.h>

#include <string>
#include <vector>

namespace vcs::assistant {

struct TabView {
  std::string title;
  unsigned uid = 0;  // stable: the tab's identity in ImGui
};

struct TabAction {
  enum Kind { None, New, Close, Switch } kind = None;
  int index = -1;
  ImVec2 plusCentre;  // where the "+" is on screen (for the tests)
};

// Draws the bar inside the current window. `active` is the model's active tab; `*want` is
// the tab the model wants ImGui to select (or -1): it is cleared here once it has been
// asked for, unless the caller sets it again after acting on the returned action.
TabAction drawChatTabs(const std::vector<TabView>& tabs, int active, int* want, bool busy, const char* newTip, float titleWidth);

// A text cut to a pixel width with "..." (whole UTF-8 characters).
std::string ellipsizeText(const std::string& text, float maxWidth);

}  // namespace vcs::assistant
