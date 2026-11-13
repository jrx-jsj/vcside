// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "project_form.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "elf_space.h"
#include "i18n.h"
#include "imgui_stdlib.h"
#include "theme_ui.h"

namespace fs = std::filesystem;

namespace vcs {

using theme::em;

static std::string joinLines(const std::vector<std::string>& lines) {
  std::string joined;
  for (auto& line : lines) joined += (joined.empty() ? "" : "\n") + line;
  return joined;
}

static std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  for (std::string line; std::getline(stream, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

bool ProjectForm::load(const std::string& path, TextEditor& text, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error) *error = F("Cannot open {0}", {path});
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  path_ = path;
  text.setText(buffer.str());
  text.setFoldSyntax(FoldSyntax::C);  // braces: the objects of the JSON fold
  error_.clear();
  dirty_ = false;
  // a file that does not read opens as text, to be mended there
  if (!project_.fromJson(buffer.str(), path, &error_)) asText_ = true;
  includeText_ = joinLines(project_.includeDirs);
  return true;
}

bool ProjectForm::showText(TextEditor& text, bool show) {
  if (show == asText_) return true;
  if (show) {
    if (dirty_) text.replaceText(project_.toJson());  // undo in the text goes back to the file's
    asText_ = true;
    return true;
  }
  Project parsed;
  if (!parsed.fromJson(text.text(), path_, &error_)) return false;
  project_ = parsed;
  includeText_ = joinLines(project_.includeDirs);
  if (text.dirty()) dirty_ = true;
  error_.clear();
  asText_ = false;
  return true;
}

bool ProjectForm::contents(TextEditor& text, std::string* jsonText, std::string* error) {
  if (!asText_) {
    *jsonText = project_.toJson();
    return true;
  }
  Project parsed;
  std::string parseError;
  if (!parsed.fromJson(text.text(), path_, &parseError)) {
    if (error) *error = parseError;
    error_ = parseError;
    return false;
  }
  project_ = parsed;
  *jsonText = text.text();
  if (jsonText->empty() || jsonText->back() != '\n') *jsonText += "\n";
  return true;
}

void ProjectForm::saved(TextEditor& text, const std::string& written) {
  if (text.text() + "\n" != written && text.text() != written) text.replaceText(written);
  text.markSaved();
  dirty_ = false;
  error_.clear();
}

void ProjectForm::render(TextEditor& text, ImFont* mono, const KnowledgeBase& kb, Toolchain& toolchain) {
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
  auto text1 = [&](const char* id, std::string& value, const char* hint = "") {
    if (ImGui::InputTextWithHint(id, hint, &value)) changed = true;
  };
  auto combo = [&](const char* id, const std::string& current, const std::vector<std::pair<std::string, std::string>>& items,
                   std::string& chosen) {
    std::string shown = current;
    for (auto& [value, label] : items)
      if (value == current) shown = label;
    if (ImGui::BeginCombo(id, shown.c_str())) {
      for (auto& [value, label] : items) {
        if (value.empty() && !label.empty()) {  // a heading
          ImGui::SeparatorText(label.c_str());
          continue;
        }
        if (ImGui::Selectable(label.c_str(), value == current)) chosen = value, changed = true;
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
    text1("##name", project_.name);
    row(T("Programming language"), nullptr);
    {
      std::string languageValue = languageId(project_.language);
      combo("##lang", languageValue, {{"dasm", T("Assembler (DASM)")}, {"batari-basic", T("batari Basic")}, {"elf-c", T("C for ELF (vcsLib)")}, {"ace-c", T("C for ACE (PlusCart, UnoCart)")}}, languageValue);
      project_.language = languageFromId(languageValue);
    }
    row(T("Main source"), T("The file the build starts from, relative to the project."));
    text1("##main", project_.main);
    row(T("ROM"), T("Where the build leaves the ROM, relative to the project. Run and debug open it."));
    text1("##output", project_.output);

    section(T("Cartridge and TV"));
    row(T("Format"), nullptr);
    {
      std::vector<std::pair<std::string, std::string>> items = {{"AUTO", T("Detected by the emulator (AUTO)")}};
      for (auto& [category, title] : formatCategories()) {
        std::vector<const CartFormat*> categoryFormats;
        for (auto& cartFormat : kb.formats())
          if (cartFormat.category == category) categoryFormats.push_back(&cartFormat);
        if (categoryFormats.empty()) continue;
        std::sort(categoryFormats.begin(), categoryFormats.end(), [](auto* left, auto* right) { return naturalLess(left->name, right->name); });
        items.push_back({"", title});
        for (auto* cartFormat : categoryFormats) items.push_back({cartFormat->id, cartFormat->name.find(cartFormat->id) != std::string::npos ? cartFormat->name : cartFormat->id + " - " + cartFormat->name});
      }
      combo("##format", project_.format, items, project_.format);
    }
    row(T("TV standard"), nullptr);
    {
      std::vector<std::pair<std::string, std::string>> items;
      auto ids = tvStandardsFor(project_.language);
      ids.push_back("AUTO");
      if (std::find(ids.begin(), ids.end(), project_.tv) == ids.end()) ids.push_back(project_.tv);
      for (auto& standard : ids) items.push_back({standard, tvLabel(standard)});
      combo("##tv", project_.tv, items, project_.tv);
    }
    row(T("Emulator"), T("Its launch options: Build > Emulator options..."));
    {
      std::vector<std::pair<std::string, std::string>> items;
      for (auto emulatorId : {"gopher2600", "stella"}) {
        Tool* tool = toolchain.get(emulatorId);
        items.push_back({emulatorId, tool ? tool->name : emulatorId});
      }
      combo("##emu", project_.emulator, items, project_.emulator);
    }
    if (project_.language == Language::Dasm) {
      row(T("Illegal opcodes"), nullptr);
      std::string illegalValue = illegalId(project_.illegal);
      combo("##illegal", illegalValue, {{"allow", T("Allow")}, {"stable-only", T("Stable only")}, {"warn", T("Warn")}, {"forbid", T("Forbid")}}, illegalValue);
      project_.illegal = illegalFromId(illegalValue);
    }

    section(T("Build"));
    row(T("Build command"), T("A command of the project's own, run in its folder (make, sh build.sh...). With &&, | or ; the system's shell runs it. Empty: VCSIDE runs the tool."));
    text1("##cmd", project_.buildCommand, project_.language == Language::BatariBasic ? T("(batari Basic)") : isC(project_.language) ? "make" : T("(DASM)"));
    if (project_.buildCommand.empty() && !isC(project_.language)) {
      const char* id = project_.language == Language::BatariBasic ? "bb" : "dasm";
      Tool* tool = toolchain.get(project_.language == Language::BatariBasic ? project_.basicTool() : id);  // PXE: the PXE beta
      std::string label = F("{0} of the project", {tool ? tool->name : id});
      row(label.c_str(), T("A path to use this project's own copy (a version of its own) instead of the one in Settings > Tools. Relative to the project or absolute."));
      std::string value = project_.toolPaths.count(id) ? project_.toolPaths[id] : "";
      std::string hint = tool && tool->found() ? F("Settings > Tools: {0}", {tool->path}) : std::string(T("not found in Settings > Tools"));
      if (ImGui::InputTextWithHint("##tool", hint.c_str(), &value)) {
        if (value.empty()) project_.toolPaths.erase(id);
        else project_.toolPaths[id] = value;
        changed = true;
      }
      if (!value.empty() && project_.toolPath(toolchain, id).empty()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", T("not found"));
      }
      if (project_.language == Language::Dasm) {
        row(T("DASM arguments"), T("Added to DASM's command line, e.g. -DPAL=1 to set a symbol."));
        text1("##flags", project_.buildFlags, "-DPAL=1");
        row(T("Include folders"), T("One per line, relative to the project: DASM looks for include and incbin files in them."));
        if (ImGui::InputTextMultiline("##inc", &includeText_, ImVec2(fieldW, ImGui::GetTextLineHeight() * 3.6f))) {
          project_.includeDirs = splitLines(includeText_);
          changed = true;
        }
      }
    }

    section(T("Space report"));
    if (project_.language == Language::ElfC) {
      row(T("Cartridge"), T("What the space report measures the game against."));
      combo("##cart", project_.elfCartridge, {{"chameleon", "Chameleon Cart"}, {"pluscart", T("PlusCart or UnoCart")}}, project_.elfCartridge);
      // the cartridge's own size shows when none is set; setting it back to that unsets it
      auto sizeField = [&](const char* id, int& kilobytes, long defaultKb) {
        int shown = kilobytes > 0 ? kilobytes : (int)defaultKb;
        if (ImGui::InputInt(id, &shown, 0)) {
          shown = std::clamp(shown, 1, 65536);
          kilobytes = shown == defaultKb ? 0 : shown;
          changed = true;
        }
      };
      if (project_.elfCartridge == "chameleon") {
        row(T("Flash (KB)"), T("128 on the Chameleon Cart; 512 on its 512 KB model."));
        sizeField("##flash", project_.elfFlashKb, defaultElfFlashKb(project_.elfCartridge));
      }
      row(T("RAM (KB)"), project_.elfCartridge == "chameleon" ? T("36 on the Chameleon Cart; 144 on its 512 KB model.")
                                                              : T("The loader's buffer: 96 in the PlusCart and UnoCart firmware."));
      sizeField("##ram", project_.elfRamKb, defaultElfRamKb(project_.elfCartridge));
    }
    row(T("Warn when fuller than"), T("A bank or a RAM area fuller than this gets a warning."));
    if (ImGui::SliderInt("##warn", &project_.spaceWarnPercent, 50, 100, "%d %%")) changed = true;
    row(T("Bytes kept for the stack"), T("Bytes of the 2600's RAM that the RAM count leaves free for the stack."));
    if (ImGui::SliderInt("##stack", &project_.stackReserve, 0, 32)) changed = true;
    ImGui::EndTable();
  }
  if (!project_.otherKeys.empty()) {
    std::string keys;
    for (auto& [key, value] : project_.otherKeys) keys += (keys.empty() ? "" : ", ") + key;
    ImGui::Dummy(ImVec2(0, em(6)));
    ImGui::TextDisabled("%s", F("Also in the file (kept as it is): {0}", {keys}).c_str());
  }
  ImGui::EndChild();
  if (changed) dirty_ = true;
}

}  // namespace vcs
