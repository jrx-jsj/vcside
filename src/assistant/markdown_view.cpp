// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/markdown_view.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "assistant/markdown.h"
#include "assistant/prompt.h"
#include "i18n.h"
#include "kb.h"
#include "theme_ui.h"

namespace vcs::assistant {

namespace {
// ---- drawing Markdown ------------------------------------------------------------------
// Words are laid out by hand so that bold, code and plain text can share a line and wrap
// together. There is no bold or italic face, so bold is drawn twice a little apart and italic
// in the dimmer text colour.

// Registers and instructions in the text open their entry in the documentation (setMarkdownLinks).
const MdLinks* gLinks = nullptr;


// The name a word stands for when it is a register or an instruction of the knowledge base ("" otherwise). In prose only
// UPPERCASE names count (the word "and" is not AND); in `inline code` any case does.
std::string linkedName(const std::string& word, bool inCode) {
  if (!gLinks || !gLinks->kb || !gLinks->open) return "";
  size_t first = 0, last = word.size();
  while (first < last && !std::isalnum((unsigned char)word[first])) first++;
  while (last > first && !std::isalnum((unsigned char)word[last - 1]) && word[last - 1] != '_') last--;
  std::string name = word.substr(first, last - first);
  if (name.size() < 3 || name.size() > 12) return "";
  for (char c : name)
    if (!std::isalnum((unsigned char)c) && c != '_') return "";
  if (!inCode && std::any_of(name.begin(), name.end(), [](char c) { return std::islower((unsigned char)c); })) return "";
  return gLinks->kb->reg(name) || gLinks->kb->instruction(name) ? name : "";
}

struct Word {
  std::string text;
  bool bold, italic, code;
};

std::vector<Word> wordsOf(const std::vector<MdRun>& runs) {
  std::vector<Word> result;
  for (auto& run : runs) {
    size_t pos = 0;
    while (pos < run.text.size()) {
      size_t end = run.text.find(' ', pos);
      end = end == std::string::npos ? run.text.size() : end + 1;  // the space stays with its word
      result.push_back({run.text.substr(pos, end - pos), run.bold, run.italic, run.code});
      pos = end;
    }
  }
  return result;
}

// Draws the runs wrapped to the width left at the cursor (a hanging indent of `indent` pixels
// for the lines after the first) and advances the layout. Returns nothing: the cursor moves.
void drawRuns(const std::vector<MdRun>& runs, ImFont* mono, float scale, bool strong, float indent, bool accent = false) {
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  ImFont* plainFont = ImGui::GetFont();
  // inline code at the prose's size (not the editor's, which follows its own zoom), a little
  // smaller as monospaced letters are wider, and on the same baseline as the words around it
  float plainSize = ImGui::GetFontSize() * scale, codeSize = plainSize * 0.92f;
  ImFont* codeFont = mono ? mono : plainFont;
  float plainAscent = plainFont->Ascent * plainSize / plainFont->FontSize;
  float codeDrop = plainAscent - codeFont->Ascent * codeSize / codeFont->FontSize;
  float lineHeight = std::round(plainSize * 1.45f);  // a little air between lines
  ImVec2 origin = ImGui::GetCursorScreenPos();
  float width = std::max(ImGui::GetContentRegionAvail().x, 40.0f);
  ImU32 textColor = ImGui::GetColorU32(accent ? theme::colv(theme::Role::UiAccent) : ImGui::GetStyleColorVec4(ImGuiCol_Text));
  ImU32 dimColor = ImGui::GetColorU32(theme::colv(theme::Role::UiTextDim));
  // inline code: the text colour at a low alpha over whatever is behind, so it shows on every theme
  ImVec4 codeTint = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  codeTint.w = 0.14f;
  ImU32 codeBackground = ImGui::GetColorU32(codeTint);
  float x = indent, y = 0;
  bool lineEmpty = true;
  const float codePad = 3.0f;  // the code box's margin, kept clear of the words beside it
  std::vector<Word> words = wordsOf(runs);
  // code boxes go under the text (channel 0), one per span and line: started at the span's
  // first word, drawn when it ends or the line wraps inside it
  drawList->ChannelsSplit(2);
  drawList->ChannelsSetCurrent(1);
  float boxLeft = 0, boxTop = 0;
  bool boxOpen = false;
  auto closeBox = [&](float right) {
    drawList->ChannelsSetCurrent(0);
    drawList->AddRectFilled(ImVec2(boxLeft, boxTop - 1), ImVec2(right, boxTop + codeSize + 1), codeBackground, 3.0f);
    drawList->ChannelsSetCurrent(1);
    boxOpen = false;
  };
  for (size_t index = 0; index < words.size(); index++) {
    const Word& word = words[index];
    bool codeContinues = word.code && index + 1 < words.size() && words[index + 1].code && word.text.back() == ' ';
    bool codeStarts = word.code && (index == 0 || !words[index - 1].code || words[index - 1].text.back() != ' ');
    ImFont* font = word.code && mono ? mono : plainFont;
    float size = word.code ? codeSize : plainSize;
    float wordWidth = font->CalcTextSizeA(size, FLT_MAX, 0.0f, word.text.c_str()).x;
    float visibleWidth = font->CalcTextSizeA(size, FLT_MAX, 0.0f, word.text.c_str(), word.text.c_str() + word.text.find_last_not_of(' ') + 1).x;
    if (codeStarts && !lineEmpty) x += codePad;
    if (!lineEmpty && x + visibleWidth > width) {
      if (boxOpen) closeBox(origin.x + x);  // the span wraps: its box ends with the line
      x = indent, y += lineHeight, lineEmpty = true;
    }
    ImVec2 position(origin.x + x, origin.y + y);
    if (word.code) {
      position.y += codeDrop;
      if (!boxOpen) boxLeft = position.x - codePad, boxTop = position.y, boxOpen = true;
      if (!codeContinues) closeBox(position.x + visibleWidth + codePad);
    }
    ImU32 color = word.italic && !accent ? dimColor : textColor;
    std::string linked = linkedName(word.text, word.code);
    ImVec2 wordMin = position, wordMax(position.x + visibleWidth, position.y + lineHeight);
    bool overLink = !linked.empty() && ImGui::IsMouseHoveringRect(wordMin, wordMax) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (!linked.empty()) color = ImGui::GetColorU32(theme::colv(theme::Role::UiAccent));
    drawList->AddText(font, size, position, color, word.text.c_str());
    if (!linked.empty()) {  // underlined; the hand and a line from the entry on hover; a click opens it
      drawList->AddLine(ImVec2(position.x, position.y + size + 1), ImVec2(position.x + visibleWidth, position.y + size + 1), color, overLink ? 1.6f : 1.0f);
      if (overLink) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        std::string summary;
        if (const Register* reg = gLinks->kb->reg(linked)) summary = reg->name + ": " + reg->summary;
        else if (const Instruction* instruction = gLinks->kb->instruction(linked)) summary = instruction->mnemonic + ": " + instruction->summary;
        ImGui::SetTooltip("%s\n%s", summary.c_str(), T("Click to open it in the documentation"));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gLinks->open(linked);
      }
    }
    if (word.bold || strong) drawList->AddText(font, size, ImVec2(position.x + 0.8f, position.y), color, word.text.c_str());
    x += wordWidth;
    if (word.code && !codeContinues) x += codePad;
    lineEmpty = false;
  }
  if (boxOpen) closeBox(origin.x + x);
  drawList->ChannelsMerge();
  ImGui::Dummy(ImVec2(width, y + lineHeight));
}

}  // namespace

void setMarkdownLinks(const MdLinks* links) { gLinks = links; }

void drawMarkdown(const std::string& text, ImFont* mono) {
  float gap = ImGui::GetTextLineHeight() * 0.35f;
  int tableIndex = 0;
  std::vector<MdBlock> blocks = parseMarkdown(text);
  auto isListItem = [](const MdBlock& block) { return block.kind == MdBlock::Bullet || block.kind == MdBlock::Number; };
  for (size_t blockIndex = 0; blockIndex < blocks.size(); blockIndex++) {
    const MdBlock& block = blocks[blockIndex];
    switch (block.kind) {
      case MdBlock::Heading: {
        static const float scales[] = {1.0f, 1.55f, 1.32f, 1.15f, 1.05f, 1.0f, 1.0f};
        ImGui::Dummy(ImVec2(0, gap * 1.5f));
        drawRuns(block.runs, mono, scales[block.level], true, 0, block.level <= 2);
        if (block.level <= 2) {  // a rule under the big headings
          ImVec2 at = ImGui::GetCursorScreenPos();
          ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y), ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y), ImGui::GetColorU32(ImGuiCol_Separator));
          ImGui::Dummy(ImVec2(0, 2.0f));
        }
        break;
      }
      case MdBlock::Bullet:
      case MdBlock::Number: {
        float em = ImGui::GetFontSize();
        float marginLeft = block.level * em * 1.2f;
        std::string marker = block.kind == MdBlock::Bullet ? std::string(block.level % 2 ? "-" : "\xe2\x80\xa2") : std::to_string(block.number) + ".";
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + marginLeft, at.y), ImGui::GetColorU32(theme::colv(theme::Role::UiTextDim)), marker.c_str());
        ImGui::Indent(marginLeft + em * 1.3f);
        drawRuns(block.runs, mono, 1.0f, false, 0);
        ImGui::Unindent(marginLeft + em * 1.3f);
        break;
      }
      case MdBlock::Quote: {
        ImVec2 at = ImGui::GetCursorScreenPos();
        float em = ImGui::GetFontSize();
        ImGui::Indent(em * 0.9f);
        ImVec2 before = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::colv(theme::Role::UiTextDim));
        drawRuns(block.runs, mono, 1.0f, false, 0);
        ImGui::PopStyleColor();
        ImGui::Unindent(em * 0.9f);
        float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x + 2, before.y), ImVec2(at.x + 5, bottom), ImGui::GetColorU32(theme::colv(theme::Role::UiAccent)));
        break;
      }
      case MdBlock::Rule: ImGui::Separator(); break;
      case MdBlock::Table: {
        size_t columns = 0;
        for (auto& row : block.rows) columns = std::max(columns, row.size());
        if (columns == 0) break;
        ImGui::PushID(tableIndex++);
        if (ImGui::BeginTable("md_table", (int)columns, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
          for (size_t r = 0; r < block.rows.size(); r++) {
            ImGui::TableNextRow();
            for (size_t c = 0; c < columns; c++) {
              ImGui::TableSetColumnIndex((int)c);
              static const std::vector<MdRun> none;
              drawRuns(c < block.rows[r].size() ? block.rows[r][c] : none, mono, 1.0f, r == 0, 0, false);
            }
          }
          ImGui::EndTable();
        }
        ImGui::PopID();
        break;
      }
      default: drawRuns(block.runs, mono, 1.0f, false, 0); break;
    }
    // the items of a list stay together; a paragraph gap after anything else
    bool listGoesOn = isListItem(block) && blockIndex + 1 < blocks.size() && isListItem(blocks[blockIndex + 1]);
    if (!listGoesOn) ImGui::Dummy(ImVec2(0, gap));
  }
}

void drawMarkdownWithCode(const std::string& text, ImFont* mono) {
  int index = 0;
  for (auto& block : splitBlocks(text)) {
    if (!block.code) {
      std::string prose = block.text;
      while (!prose.empty() && (prose.back() == '\n' || prose.back() == ' ')) prose.pop_back();
      if (!prose.empty()) drawMarkdown(prose, mono);
      continue;
    }
    if (block.text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
    ImGui::PushID(index++);
    std::string code = block.text;
    while (!code.empty() && code.back() == '\n') code.pop_back();
    float lines = 1;
    for (char c : code) lines += c == '\n';
    ImGui::PushFont(mono);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::colv(theme::Role::EdBackground));
    float height = std::min(lines, 24.0f) * ImGui::GetTextLineHeight() + ImGui::GetStyle().WindowPadding.y * 2;
    if (ImGui::BeginChild("code", ImVec2(-ImGui::GetFontSize() * 0.9f, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) ImGui::TextUnformatted(code.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() * 0.35f));
  }
}

}  // namespace vcs::assistant
