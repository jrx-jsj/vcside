// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "project_form.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "i18n.h"
#include "imgui_stdlib.h"
#include "theme_ui.h"

namespace fs = std::filesystem;

namespace vcs {

using theme::em;

static std::string joinLines(const std::vector<std::string>& v) {
  std::string s;
  for (auto& x : v) s += (s.empty() ? "" : "\n") + x;
  return s;
}

static std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string l; std::getline(in, l);) {
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    if (!l.empty()) out.push_back(l);
  }
  return out;
}

bool ProjectForm::load(const std::string& path, TextEditor& text, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = F("Cannot open {0}", {path});
    return false;
  }
  std::ostringstream s;
  s << f.rdbuf();
  path_ = path;
  text.setText(s.str());
  text.setFoldSyntax(FoldSyntax::C);  // braces: the objects of the JSON fold
  error_.clear();
  dirty_ = false;
  // a file that does not read opens as text, to be mended there
  if (!p_.fromJson(s.str(), path, &error_)) asText_ = true;
  includeText_ = joinLines(p_.includeDirs);
  return true;
}

bool ProjectForm::showText(TextEditor& text, bool on) {
  if (on == asText_) return true;
  if (on) {
    if (dirty_) text.replaceText(p_.toJson());  // undo in the text goes back to the file's
    asText_ = true;
    return true;
  }
  Project q;
  if (!q.fromJson(text.text(), path_, &error_)) return false;
  p_ = q;
  includeText_ = joinLines(p_.includeDirs);
  if (text.dirty()) dirty_ = true;
  error_.clear();
  asText_ = false;
  return true;
}

bool ProjectForm::contents(TextEditor& text, std::string* out, std::string* error) {
  if (!asText_) {
    *out = p_.toJson();
    return true;
  }
  Project q;
  std::string err;
  if (!q.fromJson(text.text(), path_, &err)) {
    if (error) *error = err;
    error_ = err;
    return false;
  }
  p_ = q;
  *out = text.text();
  if (out->empty() || out->back() != '\n') *out += "\n";
  return true;
}

void ProjectForm::saved(TextEditor& text, const std::string& written) {
  if (text.text() + "\n" != written && text.text() != written) text.replaceText(written);
  text.markSaved();
  dirty_ = false;
  error_.clear();
}

void ProjectForm::render(TextEditor& text, ImFont* mono, const KnowledgeBase& kb, Toolchain& tc) {
  // Form | Text
  ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + em(6), ImGui::GetCursorPosY() + em(4)));
  if (ImGui::RadioButton(T("Form"), !asText_)) showText(text, false);
  ImGui::SameLine();
  if (ImGui::RadioButton(T("Text"), asText_)) showText(text, true);
  if (!error_.empty()) {
    ImGui::SameLine(0, em(16));
    ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", error_.c_str());
  }
  ImGui::Separator();
  if (asText_) {
    if (text.render("##vcsproj", mono)) error_.clear();
    return;
  }

  ImGui::BeginChild("##form", ImVec2(0, 0), 0);
  bool changed = false;
  float labelW = em(170), fieldW = std::min(em(460), ImGui::GetContentRegionAvail().x - labelW - em(20));
  auto row = [&](const char* label, const char* help) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(fieldW);
  };
  auto section = [&](const char* title) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Dummy(ImVec2(0, em(6)));
    ImGui::TextDisabled("%s", title);
  };
  auto text1 = [&](const char* id, std::string& v, const char* hint = "") {
    if (ImGui::InputTextWithHint(id, hint, &v)) changed = true;
  };
  auto combo = [&](const char* id, const std::string& current, const std::vector<std::pair<std::string, std::string>>& items,
                   std::string& out) {
    std::string shown = current;
    for (auto& [v, l] : items)
      if (v == current) shown = l;
    if (ImGui::BeginCombo(id, shown.c_str())) {
      for (auto& [v, l] : items) {
        if (v.empty() && !l.empty()) {  // a heading
          ImGui::SeparatorText(l.c_str());
          continue;
        }
        if (ImGui::Selectable(l.c_str(), v == current)) out = v, changed = true;
      }
      ImGui::EndCombo();
    }
  };

  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + em(6));
  if (ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelW);
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
    section(T("Project"));
    row(T("Name"), nullptr);
    text1("##name", p_.name);
    row(T("Programming language"), nullptr);
    {
      std::string lang = languageId(p_.language);
      combo("##lang", lang, {{"dasm", T("Assembler (DASM)")}, {"batari-basic", T("batari Basic")}, {"elf-c", T("C for ELF (vcsLib)")}}, lang);
      p_.language = languageFromId(lang);
    }
    row(T("Main source"), T("The file the build starts from, relative to the project."));
    text1("##main", p_.main);
    row(T("ROM"), T("Where the build leaves the ROM, relative to the project. Run and debug open it."));
    text1("##output", p_.output);

    section(T("Cartridge and TV"));
    row(T("Format"), nullptr);
    {
      std::vector<std::pair<std::string, std::string>> items = {{"AUTO", T("Detected by the emulator (AUTO)")}};
      for (auto& [cat, title] : formatCategories()) {
        std::vector<const CartFormat*> in;
        for (auto& f : kb.formats())
          if (f.category == cat) in.push_back(&f);
        if (in.empty()) continue;
        std::sort(in.begin(), in.end(), [](auto* a, auto* b) { return naturalLess(a->name, b->name); });
        items.push_back({"", title});
        for (auto* f : in) items.push_back({f->id, f->name.find(f->id) != std::string::npos ? f->name : f->id + " - " + f->name});
      }
      combo("##format", p_.format, items, p_.format);
    }
    row(T("TV standard"), nullptr);
    {
      std::vector<std::pair<std::string, std::string>> items;
      auto ids = tvStandardsFor(p_.language);
      ids.push_back("AUTO");
      if (std::find(ids.begin(), ids.end(), p_.tv) == ids.end()) ids.push_back(p_.tv);
      for (auto& t : ids) items.push_back({t, t == "AUTO" ? T("Chosen by the emulator") : t});
      combo("##tv", p_.tv, items, p_.tv);
    }
    row(T("Emulator"), T("Its launch options: Build > Emulator options..."));
    {
      std::vector<std::pair<std::string, std::string>> items;
      for (auto e : {"stella", "gopher2600"}) {
        Tool* t = tc.get(e);
        items.push_back({e, t ? t->name : e});
      }
      combo("##emu", p_.emulator, items, p_.emulator);
    }
    if (p_.language == Language::Dasm) {
      row(T("Illegal opcodes"), nullptr);
      std::string il = illegalId(p_.illegal);
      combo("##illegal", il, {{"allow", T("Allow")}, {"stable-only", T("Stable only")}, {"warn", T("Warn")}, {"forbid", T("Forbid")}}, il);
      p_.illegal = illegalFromId(il);
    }

    section(T("Build"));
    row(T("Build command"), T("A command of the project's own, run in its folder (make, sh build.sh...). With &&, | or ; the system's shell runs it. Empty: VCSIDE runs the tool."));
    text1("##cmd", p_.buildCommand, p_.language == Language::BatariBasic ? T("(batari Basic)") : p_.language == Language::ElfC ? "make" : T("(DASM)"));
    if (p_.buildCommand.empty() && p_.language != Language::ElfC) {
      const char* id = p_.language == Language::BatariBasic ? "bb" : "dasm";
      Tool* t = tc.get(id);
      std::string label = F("{0} of the project", {t ? t->name : id});
      row(label.c_str(), T("A path to use this project's own copy (a version of its own) instead of the one in Settings > Tools. Relative to the project or absolute."));
      std::string v = p_.toolPaths.count(id) ? p_.toolPaths[id] : "";
      std::string hint = t && t->found() ? F("Settings > Tools: {0}", {t->path}) : std::string(T("not found in Settings > Tools"));
      if (ImGui::InputTextWithHint("##tool", hint.c_str(), &v)) {
        if (v.empty()) p_.toolPaths.erase(id);
        else p_.toolPaths[id] = v;
        changed = true;
      }
      if (!v.empty() && p_.toolPath(tc, id).empty()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", T("not found"));
      }
      if (p_.language == Language::Dasm) {
        row(T("DASM arguments"), T("Added to DASM's command line, e.g. -DPAL=1 to set a symbol."));
        text1("##flags", p_.buildFlags, "-DPAL=1");
        row(T("Include folders"), T("One per line, relative to the project: DASM looks for include and incbin files in them."));
        if (ImGui::InputTextMultiline("##inc", &includeText_, ImVec2(fieldW, ImGui::GetTextLineHeight() * 3.6f))) {
          p_.includeDirs = splitLines(includeText_);
          changed = true;
        }
      }
    }

    section(T("Space report"));
    row(T("Warn when fuller than"), T("A bank or a RAM area fuller than this gets a warning."));
    if (ImGui::SliderInt("##warn", &p_.spaceWarnPercent, 50, 100, "%d %%")) changed = true;
    row(T("Bytes kept for the stack"), T("Bytes of the 2600's RAM that the RAM count leaves free for the stack."));
    if (ImGui::SliderInt("##stack", &p_.stackReserve, 0, 32)) changed = true;
    ImGui::EndTable();
  }
  if (!p_.otherKeys.empty()) {
    std::string keys;
    for (auto& [k, v] : p_.otherKeys) keys += (keys.empty() ? "" : ", ") + k;
    ImGui::Dummy(ImVec2(0, em(6)));
    ImGui::TextDisabled("%s", F("Also in the file (kept as it is): {0}", {keys}).c_str());
  }
  ImGui::EndChild();
  if (changed) dirty_ = true;
}

}  // namespace vcs
