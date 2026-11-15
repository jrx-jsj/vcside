// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The Find in files panel (Edit → Find in files..., Ctrl/Cmd+Shift+F): docked with the
// Project panel, it searches the project's files with fif::Search and lists the matches
// by file; a click opens the file with the match selected.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app.h"
#include "file_icons.h"
#include "i18n.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "theme_ui.h"

namespace fs = std::filesystem;

namespace vcs {

static const int kFindResultCap = 2000;  // matches shown at most: then a note asks for a narrower search

// A file an editor shows as text (not the graphics or music editor, an image or a form).
static bool editsText(const OpenFile& file) { return !file.gfx && !file.music && !file.image && !file.form; }

static std::string relativeTo(const std::string& folder, const std::string& path) {
  std::error_code errorCode;
  fs::path relative = fs::relative(fs::path(path), fs::path(folder), errorCode);
  if (errorCode || relative.empty() || relative.native()[0] == '.') return "";
  return relative.generic_string();
}

void App::openFindInFiles() {
  if (!hasProject()) return;
  find_.focusQuery = 3;  // a tab that was hidden appears first, then takes the keyboard
  if (activeFile_ >= 0 && editsText(*files_[activeFile_])) {
    std::string selected = files_[activeFile_]->editor.selectedText();
    if (!selected.empty() && selected.find('\n') == std::string::npos && selected != find_.query) {
      find_.query = selected;
      startFindInFiles();
    }
  }
}

void App::startFindInFiles() {
  find_.changedAt = -1;
  find_.results.clear();
  find_.searched = false;
  find_.search.cancel();
  fif::Query query;
  query.text = find_.query, query.matchCase = find_.matchCase, query.wholeWord = find_.wholeWord, query.regex = find_.regex;
  query.include = find_.include, query.exclude = find_.exclude;
  find_.matcher = std::make_unique<fif::Matcher>(query);
  find_.error = find_.matcher->error();
  if (find_.query.empty() || !find_.matcher->valid() || folder_.empty()) return;
  // What the Project panel shows faded is left out: any build/ folder, what Git ignores,
  // and the project's own build and ROM folders.
  fif::Scope scope;
  scope.root = folder_;
  for (const std::string& ignored : gitIgnored_) {
    std::string relative = relativeTo(folder_, ignored);
    if (!relative.empty()) scope.skipPaths.insert(relative);
  }
  if (project_) {
    for (const std::string& outputFolder : {project_->buildFolder(), fs::path(project_->outputPath()).parent_path().string()}) {
      std::string relative = relativeTo(folder_, outputFolder);
      if (!relative.empty()) scope.skipPaths.insert(relative);
    }
  }
  scope.skipExtensions = {".vcsgfx", ".vcsmusic", ".vcsproj"};  // opened in editors of their own
  for (auto& openedFile : files_)
    if (editsText(*openedFile) && openedFile->dirty()) scope.openTexts[openedFile->path] = openedFile->editor.text();
  find_.search.start(query, scope, kFindResultCap);
  find_.searched = true;
}

void App::findResultClicked(const fif::FileResult& result, const fif::ResultLine& line, const fif::Span& span) {
  OpenFile* opened = openFile(result.path);
  if (!opened || !editsText(*opened)) return;
  opened->editor.selectRange({line.line, span.start}, {line.line, span.end});
  opened->requestFocus = true;
}

void App::replaceInFiles(int fileIndex) {
  if (!find_.matcher || !find_.matcher->valid()) return;
  int replaced = 0, filesChanged = 0;
  for (int index = 0; index < (int)find_.results.size(); index++) {
    if (fileIndex >= 0 && index != fileIndex) continue;
    const fif::FileResult& result = find_.results[index];
    int count = 0;
    OpenFile* openedFile = nullptr;
    for (auto& file : files_)
      if (file->path == result.path && editsText(*file)) openedFile = file.get();
    if (openedFile) {  // an editor has it: its text changes, and Undo brings it back
      std::string changed = fif::replaceInText(openedFile->editor.text(), *find_.matcher, find_.replacement, &count);
      if (count) openedFile->editor.replaceText(changed);
    } else {
      std::ifstream in(result.path, std::ios::binary);
      std::ostringstream contents;
      contents << in.rdbuf();
      in.close();
      std::string changed = fif::replaceInText(contents.str(), *find_.matcher, find_.replacement, &count);
      if (count) {
        std::ofstream out(result.path, std::ios::binary);
        if (!(out << changed)) {
          output_ += F("Cannot write {0}", {result.path}) + "\n";
          continue;
        }
      }
    }
    if (count) replaced += count, filesChanged++;
  }
  lastStatus_ = F("Replaced {0} matches in {1} files.", {std::to_string(replaced), std::to_string(filesChanged)});
  startFindInFiles();
}

// A small button that stays pressed while the option is on.
static bool optionToggle(const char* label, bool* value, const char* tooltip) {
  if (*value) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::colv(theme::Role::UiAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::colv(theme::Role::UiAccentHover));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::colv(theme::Role::UiBackground));
  }
  bool pressed = ImGui::Button(label);
  if (*value) ImGui::PopStyleColor(3);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
  if (pressed) *value = !*value;
  return pressed;
}

// The part of a line to show: no indentation, and when the match is far in, the text just
// before it after an ellipsis. Spans move with it; tabs show as spaces.
struct ShownLine {
  std::string text;
  std::vector<fif::Span> spans;
};
static ShownLine shownLine(const fif::ResultLine& line) {
  ShownLine shown;
  const std::string& text = line.text;
  size_t from = text.find_first_not_of(" \t");
  if (from == std::string::npos) from = 0;
  const size_t kContext = 24;  // bytes kept before the first match
  if (!line.spans.empty() && (size_t)line.spans[0].start > from + kContext) {
    from = line.spans[0].start - kContext;
    while (from > 0 && ((unsigned char)text[from] & 0xC0) == 0x80) from--;  // not inside a UTF-8 character
    shown.text = "\xE2\x80\xA6";  // …
  }
  int shift = (int)shown.text.size() - (int)from;
  size_t to = std::min(text.size(), from + 400);
  while (to < text.size() && ((unsigned char)text[to] & 0xC0) == 0x80) to++;
  shown.text += text.substr(from, to - from);
  std::replace(shown.text.begin(), shown.text.end(), '\t', ' ');
  for (const fif::Span& span : line.spans) {
    if (span.start < (int)from) continue;
    int end = std::min(span.end, (int)to);
    if (end <= span.start) continue;
    shown.spans.push_back({span.start + shift, end + shift});
  }
  return shown;
}

void App::findInFilesPanel() {
  using theme::em;
  char title[96];
  snprintf(title, sizeof title, "%s###findinfiles", T("Find in files"));
  if (find_.focusQuery > 0) ImGui::SetNextWindowFocus();
  if (!theme::beginPanel(title)) {
    theme::endPanel();
    return;
  }
  if (!hasProject() || folder_.empty()) {
    ImGui::TextDisabled("%s", T("No project open."));
    theme::endPanel();
    return;
  }
  if (find_.folder != folder_) {  // another project: the last one's results go
    find_.search.cancel();
    find_.results.clear(), find_.collapsed.clear();
    find_.searched = false;
    find_.folder = folder_;
    if (!find_.query.empty()) find_.changedAt = ImGui::GetTime();
  }
  find_.search.take(find_.results);
  double now = ImGui::GetTime();
  bool changed = false;
  float spacing = ImGui::GetStyle().ItemInnerSpacing.x;

  // the search box: replace's arrow, the text, then the options
  if (ImGui::ArrowButton("##replacetoggle", find_.showReplace ? ImGuiDir_Down : ImGuiDir_Right)) find_.showReplace = !find_.showReplace;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(find_.showReplace ? "Hide replace" : "Replace"));
  ImGui::SameLine(0, spacing);
  float togglesWidth = ImGui::CalcTextSize("Aa").x + ImGui::CalcTextSize(T("Word")).x + ImGui::CalcTextSize(".*").x +
                       ImGui::GetStyle().FramePadding.x * 6 + spacing * 3;
  ImGui::SetNextItemWidth(std::max(em(60), ImGui::GetContentRegionAvail().x - togglesWidth));
  if (find_.focusQuery > 0) {
    if (--find_.focusQuery == 0) ImGui::SetKeyboardFocusHere();
  }
  bool enter = ImGui::InputTextWithHint("##findquery", T("Search"), &find_.query, ImGuiInputTextFlags_EnterReturnsTrue);
  changed |= ImGui::IsItemEdited();
  ImGui::SameLine(0, spacing);
  changed |= optionToggle("Aa", &find_.matchCase, T("Match case"));
  ImGui::SameLine(0, spacing);
  changed |= optionToggle(T("Word"), &find_.wholeWord, T("Whole word"));
  ImGui::SameLine(0, spacing);
  changed |= optionToggle(".*", &find_.regex, T("Regular expression"));

  bool hasMatches = !find_.results.empty() && !find_.search.running();
  if (find_.showReplace) {
    float indent = ImGui::GetFrameHeight() + spacing;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    float buttonWidth = ImGui::CalcTextSize(T("Replace all")).x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(std::max(em(60), ImGui::GetContentRegionAvail().x - buttonWidth - spacing));
    ImGui::InputTextWithHint("##findreplace", find_.regex ? T("Replace ($1 for a group)") : T("Replace"), &find_.replacement);
    ImGui::SameLine(0, spacing);
    ImGui::BeginDisabled(!hasMatches);
    if (ImGui::Button(T("Replace all"))) find_.confirmReplaceAll = true;
    ImGui::EndDisabled();
  }

  // the files to look in
  if (ImGui::SmallButton(find_.showFilters ? "-##filters" : "...##filters")) find_.showFilters = !find_.showFilters;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Files to include and exclude"));
  bool filtered = !find_.include.empty() || !find_.exclude.empty();
  if (!find_.showFilters && filtered) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("Only some files"));
  }
  if (find_.showFilters) {
    ImGui::TextDisabled("%s", T("Files to include"));
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::InputTextWithHint("##findinclude", "*.asm, src/**", &find_.include);
    ImGui::TextDisabled("%s", T("Files to exclude"));
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::InputTextWithHint("##findexclude", "generated, *.lst", &find_.exclude);
  }
  if (changed) find_.changedAt = now;
  // typing searches a moment after the last key; Enter right away
  if (enter || (find_.changedAt >= 0 && now - find_.changedAt > 0.3)) startFindInFiles();

  // what was found
  int totalMatches = 0;
  for (auto& result : find_.results) totalMatches += result.matches;
  if (!find_.error.empty()) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", F("Invalid regular expression: {0}", {find_.error}).c_str());
    ImGui::PopTextWrapPos();
  } else if (find_.searched) {
    std::string count = totalMatches == 1         ? std::string(T("1 result in 1 file"))
                        : find_.results.size() == 1 ? F("{0} results in 1 file", {std::to_string(totalMatches)})
                                                    : F("{0} results in {1} files", {std::to_string(totalMatches), std::to_string(find_.results.size())});
    std::string summary = find_.search.running() ? T("Searching...") : totalMatches == 0 ? T("No results.") : count;
    if (find_.search.running() && totalMatches) summary += "  " + count;
    ImGui::TextDisabled("%s", summary.c_str());
    if (!find_.results.empty()) {
      ImGui::SameLine();
      bool allCollapsed = find_.collapsed.size() >= find_.results.size();
      if (ImGui::SmallButton(allCollapsed ? T("Expand all") : T("Collapse all"))) {
        find_.collapsed.clear();
        if (!allCollapsed)
          for (auto& result : find_.results) find_.collapsed.insert(result.path);
      }
    }
    if (find_.search.truncated() && !find_.search.running()) {
      ImGui::PushTextWrapPos(0);
      ImGui::TextColored(theme::colv(theme::Role::MsgWarning), "%s",
                         F("Only the first {0} results are shown: there are more. Narrow the search or the files to see the rest.",
                           {std::to_string(kFindResultCap)}).c_str());
      ImGui::PopTextWrapPos();
    }
  }

  // The results: a row per file, then a row per line. Rows have the same height, so only
  // the ones in view are drawn.
  struct Row {
    int file, line;  // line -1: the file's own row
  };
  std::vector<Row> rows;
  for (int fileIndex = 0; fileIndex < (int)find_.results.size(); fileIndex++) {
    rows.push_back({fileIndex, -1});
    if (!find_.collapsed.count(find_.results[fileIndex].path))
      for (int lineIndex = 0; lineIndex < (int)find_.results[fileIndex].lines.size(); lineIndex++) rows.push_back({fileIndex, lineIndex});
  }
  bool previewReplace = find_.showReplace && find_.matcher && find_.matcher->valid();
  int replaceFile = -1;
  ImGui::BeginChild("##findresults", ImVec2(0, 0), ImGuiChildFlags_None);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  float fontH = ImGui::GetTextLineHeight(), rowH = std::round(fontH * 1.45f), indentW = std::round(fontH * 0.9f);
  float iconSize = std::round(fontH * 0.95f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  ImGuiListClipper clipper;
  clipper.Begin((int)rows.size(), rowH);
  while (clipper.Step()) {
    for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; rowIndex++) {
      const Row& row = rows[rowIndex];
      const fif::FileResult& result = find_.results[row.file];
      ImVec2 origin = ImGui::GetCursorScreenPos();
      float left = ImGui::GetWindowPos().x, right = left + ImGui::GetWindowWidth() - ImGui::GetStyle().ScrollbarSize * 0.5f;
      ImGui::PushID(rowIndex);
      bool clicked = ImGui::InvisibleButton("row", ImVec2(std::max(1.0f, right - origin.x), rowH));
      bool hover = ImGui::IsItemHovered();
      ImGui::PopID();
      if (hover) drawList->AddRectFilled(ImVec2(left, origin.y), ImVec2(right, origin.y + rowH), theme::col(theme::Role::FilesRowHover));
      float centreY = origin.y + rowH * 0.5f, textY = std::round(centreY - fontH * 0.5f);
      drawList->PushClipRect(ImVec2(left, origin.y), ImVec2(right, origin.y + rowH), true);
      if (row.line < 0) {  // the file: chevron, icon, name, its folder, the count
        bool open = !find_.collapsed.count(result.path);
        float x = origin.x, chevronSize = fontH * 0.22f, cx = x + indentW * 0.5f, thickness = std::max(1.0f, fontH * 0.09f);
        ImU32 dim = theme::col(theme::Role::UiTextDim);
        if (open) {
          ImVec2 points[3] = {ImVec2(cx - chevronSize, centreY - chevronSize * 0.5f), ImVec2(cx, centreY + chevronSize * 0.5f), ImVec2(cx + chevronSize, centreY - chevronSize * 0.5f)};
          drawList->AddPolyline(points, 3, dim, 0, thickness);
        } else {
          ImVec2 points[3] = {ImVec2(cx - chevronSize * 0.5f, centreY - chevronSize), ImVec2(cx + chevronSize * 0.5f, centreY), ImVec2(cx - chevronSize * 0.5f, centreY + chevronSize)};
          drawList->AddPolyline(points, 3, dim, 0, thickness);
        }
        x += indentW;
        std::string name = fs::path(result.relative).filename().string();
        files::drawIcon(drawList, ImVec2(x, std::round(centreY - iconSize * 0.5f)), iconSize, files::kindOf(name, false));
        x += iconSize + fontH * 0.4f;
        std::string label = name + (result.unsaved ? " \xE2\x97\x8F" : "");  // ● unsaved: the editor's text
        drawList->AddText(ImVec2(x, textY), theme::col(theme::Role::UiText), label.c_str());
        x += ImGui::CalcTextSize(label.c_str()).x + fontH * 0.5f;
        std::string folder = fs::path(result.relative).parent_path().generic_string();
        std::string count = std::to_string(result.matches);
        float countW = ImGui::CalcTextSize(count.c_str()).x + fontH * 0.7f;
        float countX = right - countW - fontH * 0.4f;
        // a hovered file offers its own replace, where the count was
        bool offerReplace = previewReplace && hover;
        if (!folder.empty() && x < countX) drawList->AddText(ImVec2(x, textY), dim, folder.c_str());
        if (offerReplace) {
          const char* replaceLabel = T("Replace");
          float buttonW = ImGui::CalcTextSize(replaceLabel).x + fontH * 0.8f, buttonX = right - buttonW - fontH * 0.3f;
          ImVec2 buttonMin(buttonX, origin.y + rowH * 0.12f), buttonMax(buttonX + buttonW, origin.y + rowH * 0.88f);
          bool overButton = ImGui::IsMouseHoveringRect(buttonMin, buttonMax);
          drawList->AddRectFilled(buttonMin, buttonMax, theme::col(overButton ? theme::Role::UiButtonHover : theme::Role::UiButton), em(3));
          drawList->AddText(ImVec2(buttonX + fontH * 0.4f, textY), theme::col(theme::Role::UiText), replaceLabel);
          if (overButton) ImGui::SetTooltip("%s", F("Replace the {0} matches in {1}", {count, name}).c_str());
          if (clicked && overButton) replaceFile = row.file, clicked = false;
        } else {
          drawList->AddRectFilled(ImVec2(countX, origin.y + rowH * 0.18f), ImVec2(countX + countW, origin.y + rowH * 0.82f),
                                  theme::col(theme::Role::UiButton), rowH);
          drawList->AddText(ImVec2(countX + fontH * 0.35f, textY), theme::col(theme::Role::UiText), count.c_str());
        }
        if (hover && !offerReplace) ImGui::SetTooltip("%s", result.relative.c_str());
        if (clicked) {
          if (!find_.collapsed.erase(result.path)) find_.collapsed.insert(result.path);
        }
      } else {  // a line: its text with the matches marked (and what replaces them)
        const fif::ResultLine& line = result.lines[row.line];
        ShownLine shown = shownLine(line);
        float x = origin.x + indentW * 2;
        ImU32 textColour = theme::col(theme::Role::UiText);
        size_t drawnTo = 0;
        int underMouse = -1;  // the match the mouse is over
        float mouseX = ImGui::GetIO().MousePos.x;
        auto drawPart = [&](const std::string& part, ImU32 colour) {
          drawList->AddText(ImVec2(x, textY), colour, part.c_str());
          x += ImGui::CalcTextSize(part.c_str()).x;
        };
        for (const fif::Span& span : shown.spans) {
          drawPart(shown.text.substr(drawnTo, span.start - drawnTo), textColour);
          std::string matched = shown.text.substr(span.start, span.end - span.start);
          float matchW = ImGui::CalcTextSize(matched.c_str()).x;
          ImVec2 boxMin(x, origin.y + rowH * 0.12f), boxMax(x + matchW, origin.y + rowH * 0.88f);
          if (mouseX >= boxMin.x && mouseX < boxMax.x) underMouse = (int)(&span - shown.spans.data());
          if (previewReplace) {  // the match struck out, then its replacement
            drawList->AddRectFilled(boxMin, boxMax, (theme::col(theme::Role::MsgError) & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 60));
            drawPart(matched, textColour);
            drawList->AddLine(ImVec2(boxMin.x, centreY), ImVec2(boxMax.x, centreY), textColour, std::max(1.0f, fontH * 0.07f));
            std::string replacement = find_.regex ? find_.matcher->replace(matched, find_.replacement) : find_.replacement;
            if (find_.regex && replacement == matched) replacement = find_.replacement;
            float replacementW = ImGui::CalcTextSize(replacement.c_str()).x;
            drawList->AddRectFilled(ImVec2(x, boxMin.y), ImVec2(x + replacementW, boxMax.y),
                                    (theme::col(theme::Role::MsgOk) & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 60));
            drawPart(replacement, textColour);
          } else {
            drawList->AddRectFilled(boxMin, boxMax, theme::col(theme::Role::EdMatch), em(2));
            drawPart(matched, textColour);
          }
          drawnTo = span.end;
        }
        drawPart(shown.text.substr(drawnTo), textColour);
        if (hover) ImGui::SetTooltip("%s", F("{0}, line {1}", {result.relative, std::to_string(line.line + 1)}).c_str());
        if (clicked && !line.spans.empty()) {
          // the match under the mouse, else the line's first
          findResultClicked(result, line, line.spans[underMouse >= 0 && underMouse < (int)line.spans.size() ? underMouse : 0]);
        }
      }
      drawList->PopClipRect();
    }
  }
  clipper.End();
  ImGui::PopStyleVar();
  ImGui::EndChild();
  if (replaceFile >= 0) replaceInFiles(replaceFile);

  // Replace all: asked first, since the files on disk are written
  if (find_.confirmReplaceAll) {
    ImGui::OpenPopup("###findreplaceall");
    find_.confirmReplaceAll = false;
  }
  char dialogTitle[96];
  snprintf(dialogTitle, sizeof dialogTitle, "%s###findreplaceall", T("Replace all"));
  if (theme::beginModal(dialogTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(F("Replace {0} matches in {1} files with \"{2}\"?",
                             {std::to_string(totalMatches), std::to_string(find_.results.size()), find_.replacement}).c_str());
    ImGui::TextDisabled("%s", T("Open files change in their editor (Undo brings them back); the others are saved."));
    if (find_.search.truncated()) ImGui::TextColored(theme::colv(theme::Role::MsgWarning), "%s", T("Only the files in the list: the search stopped before the end."));
    ImGui::Spacing();
    if (theme::primaryButton(T("Replace all"))) {
      replaceInFiles(-1);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (theme::dialogButton(T("Cancel")) || theme::dialogEscape()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  theme::endPanel();
}

}  // namespace vcs
