// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/chat_tabs.h"

namespace vcs::assistant {

std::string ellipsizeText(const std::string& text, float maxWidth) {
  if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
  std::string cut = text;
  while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > maxWidth) {
    do cut.pop_back();
    while (!cut.empty() && (cut.back() & 0xC0) == 0x80);
  }
  return cut + "...";
}

TabAction drawChatTabs(const std::vector<TabView>& tabs, int active, int* want, bool busy, const char* newTip, float titleWidth) {
  TabAction action;
  int appliedWant = *want;
  int selected = -1;
  ImGui::BeginDisabled(busy);  // the answer goes to the conversation it was asked in
  if (ImGui::BeginTabBar("chat_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
    if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) action.kind = TabAction::New;
    ImVec2 low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
    action.plusCentre = ImVec2((low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", newTip);
    for (int i = 0; i < (int)tabs.size(); i++) {
      std::string label = ellipsizeText(tabs[i].title, titleWidth) + "###tab" + std::to_string(tabs[i].uid);
      bool keepOpen = true;
      if (ImGui::BeginTabItem(label.c_str(), &keepOpen, appliedWant == i ? ImGuiTabItemFlags_SetSelected : 0)) {
        selected = i;
        ImGui::EndTabItem();
      }
      if (ImGui::IsItemHovered() && ellipsizeText(tabs[i].title, titleWidth) != tabs[i].title) ImGui::SetTooltip("%s", tabs[i].title.c_str());
      if (!keepOpen && action.kind == TabAction::None) action.kind = TabAction::Close, action.index = i;
    }
    ImGui::EndTabBar();
  }
  ImGui::EndDisabled();
  // a tab ImGui selected by itself (a click): the model follows, unless a selection was asked for
  if (action.kind == TabAction::None && !busy && selected >= 0 && selected != active && appliedWant < 0) action.kind = TabAction::Switch, action.index = selected;
  if (busy) action.kind = TabAction::None;
  if (*want == appliedWant) *want = -1;
  return action;
}

}  // namespace vcs::assistant
