// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "theme_ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "i18n.h"
#include "imgui_internal.h"

namespace vcs::theme {

// ---------------------------------------------------------------------------
// ImGui style

bool beginModal(const char* name, bool* open, ImGuiWindowFlags flags) {
  ImU32 title = col(Role::DlgTitle);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, title);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, title);
  ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, title);
  ImGui::PushStyleColor(ImGuiCol_Text, col(Role::DlgTitleText));  // the title and its close button
  ImGui::PushStyleColor(ImGuiCol_PopupBg, col(Role::DlgBackground));
  ImGui::PushStyleColor(ImGuiCol_Border, col(Role::DlgBorder));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, std::max(1.0f, em(3)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, em(4));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(16), em(14)));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em(12), em(8)));  // a taller title band
  bool shown = ImGui::BeginPopupModal(name, open, flags);
  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor(6);
  if (!shown) return false;
  // the stripes, right under the title band, from border to border
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  ImRect bar = window->TitleBarRect();
  float borderInset = window->WindowBorderSize * 0.5f, stripeHeight = std::max(3.0f, em(4.5f));
  ImDrawList* drawList = window->DrawList;
  drawList->PushClipRect(window->Rect().Min, window->Rect().Max, false);
  drawStripes(drawList, ImVec2(bar.Min.x + borderInset, bar.Max.y), ImVec2(bar.Max.x - borderInset, bar.Max.y + stripeHeight * 3));
  drawList->PopClipRect();
  // scrolled content is clipped at their lower edge, so it never paints over them
  float stripesBottom = bar.Max.y + stripeHeight * 3;
  window->ClipRect.Min.y = std::max(window->ClipRect.Min.y, stripesBottom);
  window->InnerClipRect.Min.y = std::max(window->InnerClipRect.Min.y, stripesBottom);
  if (!drawList->_ClipRectStack.empty()) {
    ImVec4& clip = drawList->_ClipRectStack.back();
    clip.y = std::max(clip.y, stripesBottom);
    drawList->_CmdHeader.ClipRect = clip;
    drawList->_OnChangedClipRect();
  }
  ImGui::Dummy(ImVec2(0, stripeHeight * 3 - em(6)));  // the content starts under them
  return true;
}

bool beginPanel(const char* name, bool* open, ImGuiWindowFlags flags) {
  // docked on the last frame: the dock's tabs show the name, there is no title band.
  // (DockIsActive is false for a window alone in its dock node, so the node is what says
  // "docked": pushing the title colours then would paint the node's tab bar with them.)
  ImGuiWindow* previous = ImGui::FindWindowByName(name);
  bool floating = !previous || !previous->DockNode;
  int colours = 0, styles = 0;
  if (floating) {
    ImU32 title = col(Role::DlgTitle);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, title);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, title);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, title);
    ImGui::PushStyleColor(ImGuiCol_Text, col(Role::DlgTitleText));  // the title and its close button
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em(12), em(8)));  // a taller title band
    colours = 4, styles = 1;
  }
  // docked, each closable tab has its own ×: the dock's extra × at the end of the row would
  // close the tab in front, a second button for the same thing
  ImGuiWindowClass panelClass;
  panelClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoCloseButton;
  ImGui::SetNextWindowClass(&panelClass);
  bool shown = ImGui::Begin(name, open, flags);
  ImGui::PopStyleVar(styles);
  ImGui::PopStyleColor(colours);
  return shown;
}

namespace {
constexpr int kMenuStyleVars = 5;
void pushMenuStyle() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em(12), em(8)));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em(12), em(9)));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, em(6));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, em(4));
  ImGui::SetNextWindowSizeConstraints(ImVec2(em(200), 0), ImVec2(FLT_MAX, FLT_MAX));
}
}  // namespace

bool beginContextMenu(const char* id) {
  pushMenuStyle();
  if (ImGui::BeginPopupContextItem(id)) return true;
  ImGui::PopStyleVar(kMenuStyleVars);
  return false;
}

bool beginMenuPopup(const char* id) {
  pushMenuStyle();
  if (ImGui::BeginPopup(id)) return true;
  ImGui::PopStyleVar(kMenuStyleVars);
  return false;
}

void endMenuPopup() {
  ImGui::EndPopup();
  ImGui::PopStyleVar(kMenuStyleVars);
}

void endPanel() {
  // the stripes beside the title: in the title band of a floating window, between the
  // title and the close button; docked, in the tab row, after the tabs
  ImGuiWindow* window = ImGui::GetCurrentWindow();
  if (!window->Collapsed && !window->Hidden) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImRect bar = window->TitleBarRect();
    float left = 0, right = bar.Max.x - window->WindowBorderSize - em(10);
    bool place = true;
    if (window->DockNode) {
      ImGuiTabBar* tabs = window->DockNode ? window->DockNode->TabBar : nullptr;
      if (tabs) bar = tabs->BarRect, left = tabs->BarRect.Min.x + std::min(tabs->WidthAllTabs, tabs->BarRect.GetWidth());
      else place = false;  // a dock without tabs: no title to go beside
      // the dock's own close button, at the right end of the tab row
      if (window->DockNode->HasCloseButton) right -= ImGui::GetFontSize() + style.FramePadding.x * 2;
    } else {
      const char* titleEnd = ImGui::FindRenderedTextEnd(window->Name);
      left = bar.Min.x + style.FramePadding.x + ImGui::CalcTextSize(window->Name, titleEnd).x;
      if (window->HasCloseButton) right -= ImGui::GetFontSize() + style.FramePadding.x;
    }
    left += em(12);
    float height = stripesHeight(), middle = std::round((bar.Min.y + bar.Max.y) * 0.5f);
    if (place && right - left > em(24)) {
      ImDrawList* drawList = window->DrawList;
      drawList->PushClipRect(window->Rect().Min, window->Rect().Max, false);
      drawStripes(drawList, ImVec2(left, middle - std::round(height * 0.5f)), ImVec2(right, middle - std::round(height * 0.5f) + height));
      drawList->PopClipRect();
    }
  }
  ImGui::End();
}

bool dialogEscape() {
  return ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
}

void drawStripes(ImDrawList* drawList, const ImVec2& min, const ImVec2& max) {
  Role stripes[3] = {Role::DlgStripe1, Role::DlgStripe2, Role::DlgStripe3};
  float stripeHeight = (max.y - min.y) / 3;
  for (int i = 0; i < 3; i++)
    drawList->AddRectFilled(ImVec2(min.x, min.y + stripeHeight * i), ImVec2(max.x, i == 2 ? max.y : min.y + stripeHeight * (i + 1)), col(stripes[i]));
}

float stripesHeight() { return std::max(3.0f, std::round(em(3) * 3)); }

bool primaryButton(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, col(Role::DlgPrimary));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(Role::DlgPrimaryHover));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(Role::DlgPrimaryHover));
  ImGui::PushStyleColor(ImGuiCol_Text, col(Role::DlgPrimaryText));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em(14), ImGui::GetStyle().FramePadding.y));  // as tall as the others
  bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
  return pressed;
}

bool dialogButton(const char* label, const ImVec2& size, bool small) {
  ImGui::PushStyleColor(ImGuiCol_Button, col(Role::DlgButton));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(Role::DlgButtonHover));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(Role::DlgButtonHover));
  ImGui::PushStyleColor(ImGuiCol_Text, col(Role::DlgButtonText));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em(small ? 8 : 14), small ? 0.0f : ImGui::GetStyle().FramePadding.y));
  bool pressed = small ? ImGui::SmallButton(label) : ImGui::Button(label, size);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
  return pressed;
}

void dialogHeading(const char* text) {
  ImVec2 position = ImGui::GetCursorScreenPos();
  float width = ImGui::GetContentRegionAvail().x, pad = em(5), line = std::max(2.0f, em(3));
  float height = ImGui::GetTextLineHeight() + pad * 2;
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(position, ImVec2(position.x + width, position.y + height), col(Role::DlgTitle));
  drawList->AddRectFilled(ImVec2(position.x, position.y + height), ImVec2(position.x + width, position.y + height + line), col(Role::DlgStripe2));
  drawList->AddText(ImVec2(position.x + em(8), position.y + pad), col(Role::DlgTitleText), text);
  ImGui::Dummy(ImVec2(width, height + line));
}

void applyImGui(const Resolved& resolved) {
  ImVec4* colours = ImGui::GetStyle().Colors;
  auto themeColour = [&](Role role) { return vec(resolved.colour[(int)role]); };
  auto alpha = [](ImVec4 base, float factor) { return ImVec4(base.x, base.y, base.z, base.w * factor); };
  colours[ImGuiCol_Text] = themeColour(Role::UiText);
  colours[ImGuiCol_TextDisabled] = themeColour(Role::UiTextDim);
  colours[ImGuiCol_WindowBg] = themeColour(Role::UiBackground);
  colours[ImGuiCol_ChildBg] = themeColour(Role::UiBackground);
  colours[ImGuiCol_PopupBg] = themeColour(Role::UiPopup);
  colours[ImGuiCol_Border] = themeColour(Role::UiBorder);
  colours[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  colours[ImGuiCol_FrameBg] = themeColour(Role::UiFrame);
  colours[ImGuiCol_FrameBgHovered] = themeColour(Role::UiFrameHover);
  colours[ImGuiCol_FrameBgActive] = themeColour(Role::UiFrameActive);
  colours[ImGuiCol_TitleBg] = themeColour(Role::UiTitle);
  colours[ImGuiCol_TitleBgActive] = themeColour(Role::UiTitleActive);
  colours[ImGuiCol_TitleBgCollapsed] = themeColour(Role::UiTitle);
  colours[ImGuiCol_MenuBarBg] = themeColour(Role::UiPanel);
  colours[ImGuiCol_ScrollbarBg] = themeColour(Role::UiScrollbar);
  colours[ImGuiCol_ScrollbarGrab] = themeColour(Role::UiScrollbarGrab);
  colours[ImGuiCol_ScrollbarGrabHovered] = themeColour(Role::UiScrollbarGrabHover);
  colours[ImGuiCol_ScrollbarGrabActive] = themeColour(Role::UiScrollbarGrabHover);
  colours[ImGuiCol_CheckMark] = themeColour(Role::UiAccent);
  colours[ImGuiCol_SliderGrab] = themeColour(Role::UiAccent);
  colours[ImGuiCol_SliderGrabActive] = themeColour(Role::UiAccentHover);
  colours[ImGuiCol_Button] = themeColour(Role::UiButton);
  colours[ImGuiCol_ButtonHovered] = themeColour(Role::UiButtonHover);
  colours[ImGuiCol_ButtonActive] = themeColour(Role::UiButtonActive);
  colours[ImGuiCol_Header] = themeColour(Role::UiHeader);
  colours[ImGuiCol_HeaderHovered] = themeColour(Role::UiHeaderHover);
  colours[ImGuiCol_HeaderActive] = themeColour(Role::UiHeaderActive);
  colours[ImGuiCol_Separator] = themeColour(Role::UiBorder);
  colours[ImGuiCol_SeparatorHovered] = alpha(themeColour(Role::UiAccent), 0.78f);
  colours[ImGuiCol_SeparatorActive] = themeColour(Role::UiAccent);
  colours[ImGuiCol_ResizeGrip] = alpha(themeColour(Role::UiAccent), 0.2f);
  colours[ImGuiCol_ResizeGripHovered] = alpha(themeColour(Role::UiAccent), 0.67f);
  colours[ImGuiCol_ResizeGripActive] = alpha(themeColour(Role::UiAccent), 0.95f);
  colours[ImGuiCol_TabHovered] = themeColour(Role::UiTabHover);
  colours[ImGuiCol_Tab] = themeColour(Role::UiTab);
  colours[ImGuiCol_TabSelected] = themeColour(Role::UiTabSelected);
  colours[ImGuiCol_TabSelectedOverline] = themeColour(Role::UiAccent);
  colours[ImGuiCol_TabDimmed] = themeColour(Role::UiTab);
  colours[ImGuiCol_TabDimmedSelected] = themeColour(Role::UiTabSelected);
  colours[ImGuiCol_TabDimmedSelectedOverline] = alpha(themeColour(Role::UiAccent), 0.0f);
  // ImGui draws the drop markers at 40-100% of this colour: kept faint so the layout stays readable
  // while a panel is dragged, with the box under the mouse still the clearest
  colours[ImGuiCol_DockingPreview] = themeColour(Role::UiDockPreview);
  colours[ImGuiCol_DockingPreview].w *= 0.5f;
  colours[ImGuiCol_DockingEmptyBg] = themeColour(Role::UiPanel);
  colours[ImGuiCol_PlotLines] = themeColour(Role::UiTextDim);
  colours[ImGuiCol_PlotLinesHovered] = themeColour(Role::UiAccent);
  colours[ImGuiCol_PlotHistogram] = themeColour(Role::UiAccent);
  colours[ImGuiCol_PlotHistogramHovered] = themeColour(Role::UiAccentHover);
  colours[ImGuiCol_TableHeaderBg] = themeColour(Role::UiTableHeader);
  colours[ImGuiCol_TableBorderStrong] = themeColour(Role::UiBorder);
  colours[ImGuiCol_TableBorderLight] = alpha(themeColour(Role::UiBorder), 0.6f);
  colours[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  colours[ImGuiCol_TableRowBgAlt] = themeColour(Role::UiTableRowAlt);
  colours[ImGuiCol_TextLink] = themeColour(Role::UiLink);
  colours[ImGuiCol_TextSelectedBg] = themeColour(Role::UiSelection);
  colours[ImGuiCol_DragDropTarget] = themeColour(Role::UiAccent);
  colours[ImGuiCol_NavCursor] = themeColour(Role::UiAccent);
  colours[ImGuiCol_NavWindowingHighlight] = alpha(themeColour(Role::UiText), 0.7f);
  colours[ImGuiCol_NavWindowingDimBg] = themeColour(Role::UiModalDim);
  colours[ImGuiCol_ModalWindowDimBg] = themeColour(Role::UiModalDim);
  // the theme's own ImGui colours, by name
  for (auto& [name, rgba] : resolved.imgui)
    for (int i = 0; i < ImGuiCol_COUNT; i++)
      if (name == ImGui::GetStyleColorName(i)) colours[i] = vec(rgba);
}

// ---------------------------------------------------------------------------
// Editor

// The names of the built-in themes, for the translations (the files say them in English).
[[maybe_unused]] static const char* kBuiltInNames[] = {N_("Dark"), N_("Light")};

std::string displayName(const Theme& theme) { return theme.builtIn ? T(theme.name.c_str()) : theme.name; }

static bool inputString(const char* id, std::string* text, float width, const char* hint = nullptr) {
  char buffer[256];
  snprintf(buffer, sizeof buffer, "%s", text->c_str());
  ImGui::SetNextItemWidth(width);
  bool changed = hint ? ImGui::InputTextWithHint(id, hint, buffer, sizeof buffer) : ImGui::InputText(id, buffer, sizeof buffer);
  if (changed) *text = buffer;
  return changed;
}

// A swatch that opens a colour picker; true if the colour was changed.
static bool swatch(const char* id, Rgba* colour) {
  ImVec4 value = vec(*colour);
  bool changed = ImGui::ColorEdit4(id, &value.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                                                 ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
  if (changed) {
    auto toByte = [](float component) { return (Rgba)std::clamp((int)(component * 255 + 0.5f), 0, 255); };
    *colour = toByte(value.x) << 24 | toByte(value.y) << 16 | toByte(value.z) << 8 | toByte(value.w);
  }
  return changed;
}

void Editor::draw(Library& library, const std::string& current, const Hooks& hooks) {
  if (!open) return;
  if (selected_.empty() || !library.find(selected_)) selected_ = current;
  ImGui::SetNextWindowSize(ImVec2(em(900), em(640)), ImGuiCond_FirstUseEver);
  if (!beginPanel((std::string(T("Themes")) + "###themes").c_str(), &open)) {
    endPanel();
    return;
  }
  if (dialogEscape() && !ImGui::IsAnyItemActive()) open = false;  // closes like the dialogs
  bool changed = false;

  // the themes
  ImGui::BeginChild("list", ImVec2(em(210), 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
  ImGui::TextDisabled("%s", T("Built in"));
  for (int pass = 0; pass < 2; pass++) {
    if (pass) ImGui::Separator(), ImGui::TextDisabled("%s", T("Yours"));
    for (auto& edited : library.themes()) {
      if (edited.builtIn != (pass == 0)) continue;
      std::string label = displayName(edited) + (edited.id == current ? "  \xE2\x80\xA2" : "") + "##" + edited.id;  // a dot: in use
      if (ImGui::Selectable(label.c_str(), edited.id == selected_)) {
        selected_ = edited.id;
        message_.clear();
        if (hooks.use) hooks.use(edited.id);
      }
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();

  ImGui::BeginChild("edit");
  Theme* edited = library.find(selected_);
  if (!edited) {
    ImGui::EndChild();
    endPanel();
    return;
  }
  std::string error;
  auto adopt = [&](Theme newTheme) {  // save a new theme and select it
    if (library.save(newTheme, &error)) {
      selected_ = newTheme.id;
      if (hooks.use) hooks.use(newTheme.id);
      message_ = F("Saved in {0}", {newTheme.path});
    } else {
      message_ = error;
    }
  };
  // New and Duplicate ask for the name first ("Custom", "Custom 2"... by default).
  auto askName = [&](bool duplicate) {
    duplicate_ = duplicate;
    std::string name = T("Custom");
    for (int suffix = 2;; suffix++) {
      bool taken = false;
      for (auto& other : library.themes()) taken |= displayName(other) == name;
      if (!taken) break;
      name = std::string(T("Custom")) + " " + std::to_string(suffix);
    }
    snprintf(newName_, sizeof newName_, "%s", name.c_str());
    focusName_ = true;
    ImGui::OpenPopup("newtheme");
  };
  if (ImGui::Button(T("New, based on this one"))) askName(false);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T("An empty theme that takes every colour from this one: change only what you want, and it keeps following the base."));
  ImGui::SameLine();
  if (ImGui::Button(T("Duplicate"))) askName(true);
  if (ImGui::BeginPopup("newtheme")) {
    ImGui::TextUnformatted(duplicate_ ? F("A copy of \"{0}\"", {displayName(*edited)}).c_str()
                                      : F("A new theme based on \"{0}\"", {displayName(*edited)}).c_str());
    if (focusName_) ImGui::SetKeyboardFocusHere(), focusName_ = false;
    ImGui::SetNextItemWidth(em(260));
    bool enter = ImGui::InputText(T("name"), newName_, sizeof newName_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    std::string name = newName_;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    ImGui::BeginDisabled(name.empty());
    if ((ImGui::Button(T("Create")) || enter) && !name.empty()) {
      Theme newTheme;
      if (duplicate_) newTheme = *edited, newTheme.id.clear(), newTheme.path.clear(), newTheme.builtIn = false;
      else newTheme.base = edited->id;
      newTheme.name = name;
      adopt(newTheme);
      edited = library.find(selected_);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(edited->builtIn);
  if (ImGui::Button(T("Delete"))) ImGui::OpenPopup("delete");
  ImGui::EndDisabled();
  if (ImGui::BeginPopup("delete")) {
    ImGui::TextUnformatted(F("Delete the theme \"{0}\" and its file?", {edited->name}).c_str());
    if (ImGui::Button(T("Delete"))) {
      std::string base = edited->base.empty() ? "dark" : edited->base;
      if (library.remove(edited->id, &error)) {
        selected_ = library.find(base) ? base : "dark";
        if (hooks.use) hooks.use(selected_);
      } else {
        message_ = error;
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    edited = library.find(selected_);
    if (!edited) {
      ImGui::EndChild();
      endPanel();
      return;
    }
  }
  // sharing: files, or text to paste in a message
  ImGui::SameLine();
  if (ImGui::Button(T("Share..."))) ImGui::OpenPopup("share");
  if (ImGui::BeginPopup("share")) {
    if (ImGui::MenuItem(T("Export to a file...")) && hooks.exportFile) hooks.exportFile(edited->id);
    if (ImGui::MenuItem(T("Copy as text"))) {
      ImGui::SetClipboardText(library.exportJson(edited->id).c_str());
      message_ = T("Copied: paste it wherever you want to share it.");
    }
    ImGui::Separator();
    if (ImGui::MenuItem(T("Import a file...")) && hooks.importFile) hooks.importFile();
    if (ImGui::MenuItem(T("Paste a theme"))) {
      const char* clip = ImGui::GetClipboardText();
      std::string id;
      if (clip && library.importText(clip, "", &id, &error)) {
        selected_ = id;
        if (hooks.use) hooks.use(id);
        message_ = F("Imported: {0}", {library.find(id)->name});
      } else {
        message_ = F("The clipboard does not hold a theme: {0}", {error});
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(T("Open the themes folder")) && hooks.openFolder) hooks.openFolder(library.userDir());
    ImGui::EndPopup();
    edited = library.find(selected_);
    if (!edited) {
      ImGui::EndChild();
      endPanel();
      return;
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T("Themes are .vcstheme files. Exported or copied themes carry everything they need, except a built-in base."));
  if (!message_.empty()) ImGui::TextColored(colv(Role::MsgInfo), "%s", message_.c_str());

  if (edited->builtIn) {
    ImGui::PushStyleColor(ImGuiCol_Text, colv(Role::MsgHighlight));
    ImGui::TextWrapped("%s", T("Built-in themes cannot be changed. \"New, based on this one\" makes a theme of yours that follows it; \"Duplicate\", a full copy."));
    ImGui::PopStyleColor();
  }
  ImGui::Separator();

  Resolved resolved = library.resolve(edited->id);
  std::map<std::string, std::string> mergedPalette, mergedColours;
  library.merged(edited->base, &mergedPalette, &mergedColours);  // what the base gives
  ImGui::BeginDisabled(edited->builtIn);

  // name and base
  if (edited->builtIn) {
    std::string shown = displayName(*edited);  // translated, and read-only anyway
    inputString(T("name"), &shown, 200);
  } else if (inputString(T("name"), &edited->name, 200)) {
    changed = true;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(em(180));
  const Theme* base = library.find(edited->base);
  if (ImGui::BeginCombo(T("based on"), base ? displayName(*base).c_str() : T("(none)"))) {
    if (ImGui::Selectable(T("(none)"), edited->base.empty())) edited->base.clear(), changed = true;
    for (auto& other : library.themes()) {
      if (other.id == edited->id) continue;
      // no loops: a theme cannot build on one that builds on it
      bool loops = false;
      for (const Theme* ancestor = &other; ancestor && !loops; ancestor = ancestor->base.empty() ? nullptr : library.find(ancestor->base)) loops = ancestor->id == edited->id || ancestor->base == other.id;
      if (loops) continue;
      if (ImGui::Selectable(displayName(other).c_str(), other.id == edited->base)) edited->base = other.id, changed = true;
    }
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(em(260));
  inputString("##filter", &filter_, 260, T("Search colours"));

  auto specHelp = [] {
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T("#RRGGBB or #RRGGBBAA, $name for a palette colour, @role for another colour of the list (@ui.accent). /40 after any of them sets its alpha (#40 of 255)."));
  };

  // the palette: named colours the roles refer to
  if (filter_.empty() && ImGui::CollapsingHeader(T("Palette"), ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextDisabled("%s", T("Named colours. A colour that refers to one ($name) changes with it."));
    std::map<std::string, std::string> all = mergedPalette;
    for (auto& [key, value] : edited->palette) all[key] = value;
    std::string removeKey;
    for (auto& [key, value] : all) {
      ImGui::PushID(("pal" + key).c_str());
      bool own = edited->palette.count(key);
      Rgba colour = 0xFF00FFFF;
      library.resolveSpec(edited->id, "$" + key, &colour);
      if (swatch("##c", &colour)) edited->palette[key] = colourText(colour), changed = true;
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      if (own) ImGui::TextUnformatted(key.c_str());
      else ImGui::TextDisabled("%s", key.c_str());
      ImGui::SameLine(200);
      std::string spec = value;
      if (inputString("##spec", &spec, 180)) edited->palette[key] = spec, changed = true;
      specHelp();
      if (own) {
        ImGui::SameLine();
        if (ImGui::SmallButton(mergedPalette.count(key) ? T("Reset") : T("Remove"))) removeKey = key;
      }
      ImGui::PopID();
    }
    if (!removeKey.empty()) edited->palette.erase(removeKey), changed = true;
    ImGui::SetNextItemWidth(em(160));
    ImGui::InputTextWithHint("##newpal", T("new name"), newPalette_, sizeof newPalette_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!newPalette_[0] || all.count(newPalette_));
    if (ImGui::Button(T("Add colour"))) edited->palette[newPalette_] = "#808080", newPalette_[0] = 0, changed = true;
    ImGui::EndDisabled();
  }

  // the roles, by group
  const char* group = nullptr;
  bool groupOpen = false;
  std::string needle = filter_;
  std::transform(needle.begin(), needle.end(), needle.begin(), ::tolower);
  for (int i = 0; i < kRoles; i++) {
    const RoleInfo& roleInfo = info((Role)i);
    std::string label = T(roleInfo.label), haystack = label + " " + roleInfo.id;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
    if (!needle.empty() && haystack.find(needle) == std::string::npos) continue;
    if (!group || std::strcmp(group, roleInfo.group) != 0) {
      group = roleInfo.group;
      groupOpen = ImGui::CollapsingHeader(T(roleInfo.group), needle.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen);
    }
    if (!groupOpen) continue;
    ImGui::PushID(roleInfo.id);
    bool own = edited->colours.count(roleInfo.id);
    Rgba colour = resolved.colour[i];
    if (swatch("##c", &colour)) edited->colours[roleInfo.id] = colourText(colour), changed = true;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (own) ImGui::TextUnformatted(label.c_str());
    else ImGui::TextDisabled("%s", label.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", roleInfo.id);
    ImGui::SameLine(330);
    std::string spec = own ? edited->colours[roleInfo.id] : "";
    std::string inherited = mergedColours.count(roleInfo.id) ? mergedColours[roleInfo.id] : "";
    if (inputString("##spec", &spec, 170, inherited.c_str())) {
      if (spec.empty()) edited->colours.erase(roleInfo.id);
      else edited->colours[roleInfo.id] = spec;
      changed = true;
    }
    specHelp();
    if (own && !edited->base.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton(T("Reset"))) edited->colours.erase(roleInfo.id), changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Back to the base theme's colour."));
    }
    ImGui::PopID();
  }

  // indentation levels in the code editor
  if (filter_.empty() && ImGui::CollapsingHeader(T("Indentation levels"))) {
    ImGui::TextDisabled("%s", T("The code editor shades each level of indentation with the next colour (none: no shading)."));
    if (ImGui::Checkbox(T("Colours of its own"), &edited->hasIndent)) {
      if (edited->hasIndent && edited->indent.empty())
        for (Rgba colour : resolved.indent) edited->indent.push_back(colourText(colour));
      changed = true;
    }
    if (edited->hasIndent) {
      int remove = -1;
      for (size_t index = 0; index < edited->indent.size(); index++) {
        ImGui::PushID((int)index);
        Rgba colour = index < resolved.indent.size() ? resolved.indent[index] : 0;
        if (swatch("##c", &colour)) edited->indent[index] = colourText(colour), changed = true;
        ImGui::SameLine();
        if (inputString("##spec", &edited->indent[index], 170)) changed = true;
        specHelp();
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Remove"))) remove = (int)index;
        ImGui::PopID();
      }
      if (remove >= 0) edited->indent.erase(edited->indent.begin() + remove), changed = true;
      if (ImGui::Button(T("Add level"))) edited->indent.push_back("#80808020"), changed = true;
    }
  }
  ImGui::EndDisabled();

  if (!resolved.problems.empty()) {
    ImGui::Separator();
    for (auto& problem : resolved.problems) ImGui::TextColored(colv(Role::MsgWarning), "%s", problem.c_str());
  }
  if (!resolved.notes.empty()) {
    ImGui::Separator();
    for (auto& note : resolved.notes) ImGui::TextDisabled("%s", note.c_str());
  }
  ImGui::EndChild();
  endPanel();

  // changes show at once; the file is written when no field is being dragged or typed in
  if (changed && !edited->builtIn) {
    dirty_ = true;
    if (hooks.use) hooks.use(edited->id);
  }
  if (dirty_ && !ImGui::IsAnyItemActive()) {
    if (Theme* existing = library.find(selected_); existing && !existing->builtIn && !library.save(*existing, &error)) message_ = error;
    dirty_ = false;
  }
}

}  // namespace vcs::theme
