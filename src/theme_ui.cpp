// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "theme_ui.h"

#include <algorithm>
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
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  ImRect bar = w->TitleBarRect();
  float b = w->WindowBorderSize * 0.5f, h = std::max(2.0f, em(2.5f));
  ImDrawList* dl = w->DrawList;
  dl->PushClipRect(w->Rect().Min, w->Rect().Max, false);
  Role stripes[3] = {Role::DlgStripe1, Role::DlgStripe2, Role::DlgStripe3};
  for (int i = 0; i < 3; i++)
    dl->AddRectFilled(ImVec2(bar.Min.x + b, bar.Max.y + h * i), ImVec2(bar.Max.x - b, bar.Max.y + h * (i + 1)), col(stripes[i]));
  dl->PopClipRect();
  ImGui::Dummy(ImVec2(0, h * 3 - em(6)));  // the content starts under them
  return true;
}

bool primaryButton(const char* label, const ImVec2& size) {
  ImGui::PushStyleColor(ImGuiCol_Button, col(Role::DlgPrimary));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(Role::DlgPrimaryHover));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(Role::DlgPrimaryHover));
  ImGui::PushStyleColor(ImGuiCol_Text, col(Role::DlgPrimaryText));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em(14), ImGui::GetStyle().FramePadding.y));  // as tall as the others
  bool r = ImGui::Button(label, size);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
  return r;
}

void applyImGui(const Resolved& r) {
  ImVec4* c = ImGui::GetStyle().Colors;
  auto v = [&](Role role) { return vec(r.colour[(int)role]); };
  auto alpha = [](ImVec4 x, float a) { return ImVec4(x.x, x.y, x.z, x.w * a); };
  c[ImGuiCol_Text] = v(Role::UiText);
  c[ImGuiCol_TextDisabled] = v(Role::UiTextDim);
  c[ImGuiCol_WindowBg] = v(Role::UiBackground);
  c[ImGuiCol_ChildBg] = v(Role::UiBackground);
  c[ImGuiCol_PopupBg] = v(Role::UiPopup);
  c[ImGuiCol_Border] = v(Role::UiBorder);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = v(Role::UiFrame);
  c[ImGuiCol_FrameBgHovered] = v(Role::UiFrameHover);
  c[ImGuiCol_FrameBgActive] = v(Role::UiFrameActive);
  c[ImGuiCol_TitleBg] = v(Role::UiTitle);
  c[ImGuiCol_TitleBgActive] = v(Role::UiTitleActive);
  c[ImGuiCol_TitleBgCollapsed] = v(Role::UiTitle);
  c[ImGuiCol_MenuBarBg] = v(Role::UiPanel);
  c[ImGuiCol_ScrollbarBg] = v(Role::UiScrollbar);
  c[ImGuiCol_ScrollbarGrab] = v(Role::UiScrollbarGrab);
  c[ImGuiCol_ScrollbarGrabHovered] = v(Role::UiScrollbarGrabHover);
  c[ImGuiCol_ScrollbarGrabActive] = v(Role::UiScrollbarGrabHover);
  c[ImGuiCol_CheckMark] = v(Role::UiAccent);
  c[ImGuiCol_SliderGrab] = v(Role::UiAccent);
  c[ImGuiCol_SliderGrabActive] = v(Role::UiAccentHover);
  c[ImGuiCol_Button] = v(Role::UiButton);
  c[ImGuiCol_ButtonHovered] = v(Role::UiButtonHover);
  c[ImGuiCol_ButtonActive] = v(Role::UiButtonActive);
  c[ImGuiCol_Header] = v(Role::UiHeader);
  c[ImGuiCol_HeaderHovered] = v(Role::UiHeaderHover);
  c[ImGuiCol_HeaderActive] = v(Role::UiHeaderActive);
  c[ImGuiCol_Separator] = v(Role::UiBorder);
  c[ImGuiCol_SeparatorHovered] = alpha(v(Role::UiAccent), 0.78f);
  c[ImGuiCol_SeparatorActive] = v(Role::UiAccent);
  c[ImGuiCol_ResizeGrip] = alpha(v(Role::UiAccent), 0.2f);
  c[ImGuiCol_ResizeGripHovered] = alpha(v(Role::UiAccent), 0.67f);
  c[ImGuiCol_ResizeGripActive] = alpha(v(Role::UiAccent), 0.95f);
  c[ImGuiCol_TabHovered] = v(Role::UiTabHover);
  c[ImGuiCol_Tab] = v(Role::UiTab);
  c[ImGuiCol_TabSelected] = v(Role::UiTabSelected);
  c[ImGuiCol_TabSelectedOverline] = v(Role::UiAccent);
  c[ImGuiCol_TabDimmed] = v(Role::UiTab);
  c[ImGuiCol_TabDimmedSelected] = v(Role::UiTabSelected);
  c[ImGuiCol_TabDimmedSelectedOverline] = alpha(v(Role::UiAccent), 0.0f);
  c[ImGuiCol_DockingPreview] = v(Role::UiDockPreview);
  c[ImGuiCol_DockingEmptyBg] = v(Role::UiPanel);
  c[ImGuiCol_PlotLines] = v(Role::UiTextDim);
  c[ImGuiCol_PlotLinesHovered] = v(Role::UiAccent);
  c[ImGuiCol_PlotHistogram] = v(Role::UiAccent);
  c[ImGuiCol_PlotHistogramHovered] = v(Role::UiAccentHover);
  c[ImGuiCol_TableHeaderBg] = v(Role::UiTableHeader);
  c[ImGuiCol_TableBorderStrong] = v(Role::UiBorder);
  c[ImGuiCol_TableBorderLight] = alpha(v(Role::UiBorder), 0.6f);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = v(Role::UiTableRowAlt);
  c[ImGuiCol_TextLink] = v(Role::UiLink);
  c[ImGuiCol_TextSelectedBg] = v(Role::UiSelection);
  c[ImGuiCol_DragDropTarget] = v(Role::UiAccent);
  c[ImGuiCol_NavCursor] = v(Role::UiAccent);
  c[ImGuiCol_NavWindowingHighlight] = alpha(v(Role::UiText), 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = v(Role::UiModalDim);
  c[ImGuiCol_ModalWindowDimBg] = v(Role::UiModalDim);
  // the theme's own ImGui colours, by name
  for (auto& [name, rgba] : r.imgui)
    for (int i = 0; i < ImGuiCol_COUNT; i++)
      if (name == ImGui::GetStyleColorName(i)) c[i] = vec(rgba);
}

// ---------------------------------------------------------------------------
// Editor

// The names of the built-in themes, for the translations (the files say them in English).
[[maybe_unused]] static const char* kBuiltInNames[] = {N_("Dark"), N_("Light")};

std::string displayName(const Theme& t) { return t.builtIn ? T(t.name.c_str()) : t.name; }

static bool inputString(const char* id, std::string* s, float width, const char* hint = nullptr) {
  char buf[256];
  snprintf(buf, sizeof buf, "%s", s->c_str());
  ImGui::SetNextItemWidth(width);
  bool changed = hint ? ImGui::InputTextWithHint(id, hint, buf, sizeof buf) : ImGui::InputText(id, buf, sizeof buf);
  if (changed) *s = buf;
  return changed;
}

// A swatch that opens a colour picker; true if the colour was changed.
static bool swatch(const char* id, Rgba* c) {
  ImVec4 v = vec(*c);
  bool changed = ImGui::ColorEdit4(id, &v.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                                                 ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
  if (changed) {
    auto b = [](float x) { return (Rgba)std::clamp((int)(x * 255 + 0.5f), 0, 255); };
    *c = b(v.x) << 24 | b(v.y) << 16 | b(v.z) << 8 | b(v.w);
  }
  return changed;
}

void Editor::draw(Library& lib, const std::string& current, const Hooks& h) {
  if (!open) return;
  if (selected_.empty() || !lib.find(selected_)) selected_ = current;
  ImGui::SetNextWindowSize(ImVec2(em(900), em(640)), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin((std::string(T("Themes")) + "###themes").c_str(), &open)) {
    ImGui::End();
    return;
  }
  bool changed = false;

  // the themes
  ImGui::BeginChild("list", ImVec2(em(210), 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
  ImGui::TextDisabled("%s", T("Built in"));
  for (int pass = 0; pass < 2; pass++) {
    if (pass) ImGui::Separator(), ImGui::TextDisabled("%s", T("Yours"));
    for (auto& t : lib.themes()) {
      if (t.builtIn != (pass == 0)) continue;
      std::string label = displayName(t) + (t.id == current ? "  \xE2\x80\xA2" : "") + "##" + t.id;  // a dot: in use
      if (ImGui::Selectable(label.c_str(), t.id == selected_)) {
        selected_ = t.id;
        message_.clear();
        if (h.use) h.use(t.id);
      }
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();

  ImGui::BeginChild("edit");
  Theme* t = lib.find(selected_);
  if (!t) {
    ImGui::EndChild();
    ImGui::End();
    return;
  }
  std::string err;
  auto adopt = [&](Theme n) {  // save a new theme and select it
    if (lib.save(n, &err)) {
      selected_ = n.id;
      if (h.use) h.use(n.id);
      message_ = F("Saved in {0}", {n.path});
    } else {
      message_ = err;
    }
  };
  // New and Duplicate ask for the name first ("Custom", "Custom 2"... by default).
  auto askName = [&](bool duplicate) {
    duplicate_ = duplicate;
    std::string name = T("Custom");
    for (int n = 2;; n++) {
      bool taken = false;
      for (auto& o : lib.themes()) taken |= displayName(o) == name;
      if (!taken) break;
      name = std::string(T("Custom")) + " " + std::to_string(n);
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
    ImGui::TextUnformatted(duplicate_ ? F("A copy of \"{0}\"", {displayName(*t)}).c_str()
                                      : F("A new theme based on \"{0}\"", {displayName(*t)}).c_str());
    if (focusName_) ImGui::SetKeyboardFocusHere(), focusName_ = false;
    ImGui::SetNextItemWidth(em(260));
    bool enter = ImGui::InputText(T("name"), newName_, sizeof newName_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    std::string name = newName_;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    ImGui::BeginDisabled(name.empty());
    if ((ImGui::Button(T("Create")) || enter) && !name.empty()) {
      Theme n;
      if (duplicate_) n = *t, n.id.clear(), n.path.clear(), n.builtIn = false;
      else n.base = t->id;
      n.name = name;
      adopt(n);
      t = lib.find(selected_);
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(t->builtIn);
  if (ImGui::Button(T("Delete"))) ImGui::OpenPopup("delete");
  ImGui::EndDisabled();
  if (ImGui::BeginPopup("delete")) {
    ImGui::TextUnformatted(F("Delete the theme \"{0}\" and its file?", {t->name}).c_str());
    if (ImGui::Button(T("Delete"))) {
      std::string base = t->base.empty() ? "dark" : t->base;
      if (lib.remove(t->id, &err)) {
        selected_ = lib.find(base) ? base : "dark";
        if (h.use) h.use(selected_);
      } else {
        message_ = err;
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    t = lib.find(selected_);
    if (!t) {
      ImGui::EndChild();
      ImGui::End();
      return;
    }
  }
  // sharing: files, or text to paste in a message
  ImGui::SameLine();
  if (ImGui::Button(T("Share..."))) ImGui::OpenPopup("share");
  if (ImGui::BeginPopup("share")) {
    if (ImGui::MenuItem(T("Export to a file...")) && h.exportFile) h.exportFile(t->id);
    if (ImGui::MenuItem(T("Copy as text"))) {
      ImGui::SetClipboardText(lib.exportJson(t->id).c_str());
      message_ = T("Copied: paste it wherever you want to share it.");
    }
    ImGui::Separator();
    if (ImGui::MenuItem(T("Import a file...")) && h.importFile) h.importFile();
    if (ImGui::MenuItem(T("Paste a theme"))) {
      const char* clip = ImGui::GetClipboardText();
      std::string id;
      if (clip && lib.importText(clip, "", &id, &err)) {
        selected_ = id;
        if (h.use) h.use(id);
        message_ = F("Imported: {0}", {lib.find(id)->name});
      } else {
        message_ = F("The clipboard does not hold a theme: {0}", {err});
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(T("Open the themes folder")) && h.openFolder) h.openFolder(lib.userDir());
    ImGui::EndPopup();
    t = lib.find(selected_);
    if (!t) {
      ImGui::EndChild();
      ImGui::End();
      return;
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T("Themes are .vcstheme files. Exported or copied themes carry everything they need, except a built-in base."));
  if (!message_.empty()) ImGui::TextColored(colv(Role::MsgInfo), "%s", message_.c_str());

  if (t->builtIn) {
    ImGui::PushStyleColor(ImGuiCol_Text, colv(Role::MsgHighlight));
    ImGui::TextWrapped("%s", T("Built-in themes cannot be changed. \"New, based on this one\" makes a theme of yours that follows it; \"Duplicate\", a full copy."));
    ImGui::PopStyleColor();
  }
  ImGui::Separator();

  Resolved res = lib.resolve(t->id);
  std::map<std::string, std::string> mergedPalette, mergedColours;
  lib.merged(t->base, &mergedPalette, &mergedColours);  // what the base gives
  ImGui::BeginDisabled(t->builtIn);

  // name and base
  if (t->builtIn) {
    std::string shown = displayName(*t);  // translated, and read-only anyway
    inputString(T("name"), &shown, 200);
  } else if (inputString(T("name"), &t->name, 200)) {
    changed = true;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(em(180));
  const Theme* base = lib.find(t->base);
  if (ImGui::BeginCombo(T("based on"), base ? displayName(*base).c_str() : T("(none)"))) {
    if (ImGui::Selectable(T("(none)"), t->base.empty())) t->base.clear(), changed = true;
    for (auto& o : lib.themes()) {
      if (o.id == t->id) continue;
      // no loops: a theme cannot build on one that builds on it
      bool loops = false;
      for (const Theme* b = &o; b && !loops; b = b->base.empty() ? nullptr : lib.find(b->base)) loops = b->id == t->id || b->base == o.id;
      if (loops) continue;
      if (ImGui::Selectable(displayName(o).c_str(), o.id == t->base)) t->base = o.id, changed = true;
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
    for (auto& [k, v] : t->palette) all[k] = v;
    std::string removeKey;
    for (auto& [k, v] : all) {
      ImGui::PushID(("pal" + k).c_str());
      bool own = t->palette.count(k);
      Rgba c = 0xFF00FFFF;
      lib.resolveSpec(t->id, "$" + k, &c);
      if (swatch("##c", &c)) t->palette[k] = colourText(c), changed = true;
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      if (own) ImGui::TextUnformatted(k.c_str());
      else ImGui::TextDisabled("%s", k.c_str());
      ImGui::SameLine(200);
      std::string spec = v;
      if (inputString("##spec", &spec, 180)) t->palette[k] = spec, changed = true;
      specHelp();
      if (own) {
        ImGui::SameLine();
        if (ImGui::SmallButton(mergedPalette.count(k) ? T("Reset") : T("Remove"))) removeKey = k;
      }
      ImGui::PopID();
    }
    if (!removeKey.empty()) t->palette.erase(removeKey), changed = true;
    ImGui::SetNextItemWidth(em(160));
    ImGui::InputTextWithHint("##newpal", T("new name"), newPalette_, sizeof newPalette_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!newPalette_[0] || all.count(newPalette_));
    if (ImGui::Button(T("Add colour"))) t->palette[newPalette_] = "#808080", newPalette_[0] = 0, changed = true;
    ImGui::EndDisabled();
  }

  // the roles, by group
  const char* group = nullptr;
  bool groupOpen = false;
  std::string f = filter_;
  std::transform(f.begin(), f.end(), f.begin(), ::tolower);
  for (int i = 0; i < kRoles; i++) {
    const RoleInfo& ri = info((Role)i);
    std::string label = T(ri.label), hay = label + " " + ri.id;
    std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
    if (!f.empty() && hay.find(f) == std::string::npos) continue;
    if (!group || std::strcmp(group, ri.group) != 0) {
      group = ri.group;
      groupOpen = ImGui::CollapsingHeader(T(ri.group), f.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen);
    }
    if (!groupOpen) continue;
    ImGui::PushID(ri.id);
    bool own = t->colours.count(ri.id);
    Rgba c = res.colour[i];
    if (swatch("##c", &c)) t->colours[ri.id] = colourText(c), changed = true;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (own) ImGui::TextUnformatted(label.c_str());
    else ImGui::TextDisabled("%s", label.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ri.id);
    ImGui::SameLine(330);
    std::string spec = own ? t->colours[ri.id] : "";
    std::string inherited = mergedColours.count(ri.id) ? mergedColours[ri.id] : "";
    if (inputString("##spec", &spec, 170, inherited.c_str())) {
      if (spec.empty()) t->colours.erase(ri.id);
      else t->colours[ri.id] = spec;
      changed = true;
    }
    specHelp();
    if (own && !t->base.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton(T("Reset"))) t->colours.erase(ri.id), changed = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Back to the base theme's colour."));
    }
    ImGui::PopID();
  }

  // indentation levels in the code editor
  if (filter_.empty() && ImGui::CollapsingHeader(T("Indentation levels"))) {
    ImGui::TextDisabled("%s", T("The code editor shades each level of indentation with the next colour (none: no shading)."));
    if (ImGui::Checkbox(T("Colours of its own"), &t->hasIndent)) {
      if (t->hasIndent && t->indent.empty())
        for (Rgba c : res.indent) t->indent.push_back(colourText(c));
      changed = true;
    }
    if (t->hasIndent) {
      int remove = -1;
      for (size_t k = 0; k < t->indent.size(); k++) {
        ImGui::PushID((int)k);
        Rgba c = k < res.indent.size() ? res.indent[k] : 0;
        if (swatch("##c", &c)) t->indent[k] = colourText(c), changed = true;
        ImGui::SameLine();
        if (inputString("##spec", &t->indent[k], 170)) changed = true;
        specHelp();
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Remove"))) remove = (int)k;
        ImGui::PopID();
      }
      if (remove >= 0) t->indent.erase(t->indent.begin() + remove), changed = true;
      if (ImGui::Button(T("Add level"))) t->indent.push_back("#80808020"), changed = true;
    }
  }
  ImGui::EndDisabled();

  if (!res.problems.empty()) {
    ImGui::Separator();
    for (auto& p : res.problems) ImGui::TextColored(colv(Role::MsgWarning), "%s", p.c_str());
  }
  if (!res.notes.empty()) {
    ImGui::Separator();
    for (auto& n : res.notes) ImGui::TextDisabled("%s", n.c_str());
  }
  ImGui::EndChild();
  ImGui::End();

  // changes show at once; the file is written when no field is being dragged or typed in
  if (changed && !t->builtIn) {
    dirty_ = true;
    if (h.use) h.use(t->id);
  }
  if (dirty_ && !ImGui::IsAnyItemActive()) {
    if (Theme* s = lib.find(selected_); s && !s->builtIn && !lib.save(*s, &err)) message_ = err;
    dirty_ = false;
  }
}

}  // namespace vcs::theme
