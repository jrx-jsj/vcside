// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "gfx_editor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "i18n.h"
#include "theme_ui.h"
#include "imgui_stdlib.h"

namespace fs = std::filesystem;

namespace vcs {

using namespace gfx;
using theme::em;

// A colour clock is wider on the screen than a scanline is tall; this keeps the
// canvas and the preview close to what a TV shows.
static constexpr float kLineAspect = 0.6f;

static ImU32 col(Tv tv, int colour, float alpha = 1.0f) {
  uint32_t rgbColour = rgb(tv, colour);
  return IM_COL32((rgbColour >> 16) & 255, (rgbColour >> 8) & 255, rgbColour & 255, (int)(alpha * 255));
}

static ImU32 mix(ImU32 from, ImU32 to, float amount) {
  auto channel = [&](int shift) { return (int)(((from >> shift) & 255) * (1 - amount) + ((to >> shift) & 255) * amount) << shift; };
  return (ImU32)(channel(0) | channel(8) | channel(16) | (255u << 24));
}

static std::string readAll(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

Asset GfxEditor::blank() {
  Asset asset;
  Sprite sprite;
  sprite.name = "sprite";
  Layer layer;
  layer.obj = Obj::P0;
  layer.resize(1, 8);
  layer.colours[0][0] = 0x1E;
  sprite.layers.push_back(layer);
  asset.sprites.push_back(sprite);
  return asset;
}

bool GfxEditor::load(const std::string& path, std::string* error) {
  path_ = path;
  std::error_code ec;
  if (!fs::exists(path, ec) || fs::file_size(path, ec) == 0) {
    asset_ = blank();
  } else if (!fromJson(readAll(path), &asset_, error)) {
    return false;
  }
  if (asset_.sprites.empty() && asset_.playfields.empty()) asset_ = blank();
  committed_ = asset_;
  undo_.clear();
  redo_.clear();
  dirty_ = changed_ = false;
  item_ = layer_ = frame_ = 0;
  return true;
}

Profile GfxEditor::profile(const Profile& project) const {
  Kernel kernel;
  if (asset_.kernel.empty() || !kernelFromId(asset_.kernel, &kernel) || kernel == project.kernel) return project;
  Profile result;
  result.kernel = kernel;
  return result;
}

static bool endsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool GfxEditor::save(const std::string& path, const Profile& project, std::string* error, std::string* generated, const std::string& outputFolder) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (error) *error = F("Cannot write {0}", {path});
    return false;
  }
  out << toJson(asset_);
  out.close();
  path_ = path;
  if (Profile kernelProfile = profile(project); !kernelProfile.basic()) {
    // the tables for the program, in the generated folder or next to the .vcsgfx
    fs::path sourcePath(path);
    fs::path folder = outputFolder.empty() ? sourcePath.parent_path() : fs::path(outputFolder);
    std::error_code ec;
    if (!folder.empty()) fs::create_directories(folder, ec);
    std::string source = sourcePath.filename().string();
    auto files = generatedFiles(asset_, source, kernelProfile.usesC);
    std::set<std::string> now;
    for (auto& generatedFile : files) {
      std::string includePath = (folder / generatedFile.name).string();
      std::ofstream output(includePath, std::ios::binary);
      if (!output) continue;
      output << generatedFile.text;
      now.insert(generatedFile.name);
      if (generated && generated->empty()) *generated = includePath;
    }
    // what an earlier split generated from this file and no longer does
    std::string stem = sourcePath.stem().string() + ".";
    for (auto& entry : fs::directory_iterator(folder.empty() ? fs::path(".") : folder, ec)) {
      std::string filename = entry.path().filename().string();
      bool ours = filename.rfind(stem, 0) == 0 && (endsWith(filename, ".gfx.asm") || endsWith(filename, ".gfx.h"));
      if (!ours || now.count(filename)) continue;
      std::ifstream input(entry.path(), std::ios::binary);
      std::string first;
      std::getline(input, first);
      input.close();
      if (generatedFrom(first, source)) fs::remove(entry.path(), ec);
    }
  }
  dirty_ = false;
  return true;
}

void GfxEditor::undo() {
  if (undo_.empty()) return;
  redo_.push_back(asset_);
  asset_ = undo_.back();
  undo_.pop_back();
  committed_ = asset_;
  dirty_ = true;
}

void GfxEditor::redo() {
  if (redo_.empty()) return;
  undo_.push_back(asset_);
  asset_ = redo_.back();
  redo_.pop_back();
  committed_ = asset_;
  dirty_ = true;
}

// A change is one undo step once the mouse is up and no field is being edited.
void GfxEditor::endFrame() {
  if (!changed_ || ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsAnyItemActive())
    return;
  undo_.push_back(committed_);
  if (undo_.size() > 200) undo_.erase(undo_.begin());
  redo_.clear();
  committed_ = asset_;
  changed_ = false;
}

// ---------------------------------------------------------------------------
// Drawing tools: the cells each one covers, in pixel coordinates.

using Cells = std::vector<std::pair<int, int>>;  // x, y

static Cells lineCells(int x0, int y0, int x1, int y1) {
  Cells cells;
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1, error = dx + dy;
  for (;;) {
    cells.push_back({x0, y0});
    if (x0 == x1 && y0 == y1) break;
    int doubleError = 2 * error;
    if (doubleError >= dy) error += dy, x0 += sx;
    if (doubleError <= dx) error += dx, y0 += sy;
  }
  return cells;
}

static Cells rectCells(int x0, int y0, int x1, int y1, bool filled) {
  Cells cells;
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      if (filled || y == y0 || y == y1 || x == x0 || x == x1) cells.push_back({x, y});
  return cells;
}

// The cells whose centre falls inside the ellipse that fits the box; the outline keeps
// those with a neighbour outside, so it stays one pixel thick and closed.
static Cells ellipseCells(int x0, int y0, int x1, int y1, bool filled) {
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  float cx = (x0 + x1 + 1) * 0.5f, cy = (y0 + y1 + 1) * 0.5f, rx = (x1 - x0 + 1) * 0.5f, ry = (y1 - y0 + 1) * 0.5f;
  auto inside = [&](int x, int y) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    float unitX = (x + 0.5f - cx) / rx, unitY = (y + 0.5f - cy) / ry;
    return unitX * unitX + unitY * unitY <= 1.0f;
  };
  Cells cells;
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      if (inside(x, y) && (filled || !inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) || !inside(x, y + 1)))
        cells.push_back({x, y});
  return cells;
}

// Every cell joined to (x, y) that has the same value, within width x height.
static Cells fillCells(int x, int y, int width, int height, const std::function<bool(int, int)>& get) {
  Cells cells;
  if (x < 0 || y < 0 || x >= width || y >= height) return cells;
  bool target = get(x, y);
  std::vector<char> seen((size_t)width * height, 0);
  std::vector<std::pair<int, int>> todo = {{x, y}};
  while (!todo.empty()) {
    auto [px, py] = todo.back();
    todo.pop_back();
    if (px < 0 || py < 0 || px >= width || py >= height || seen[(size_t)py * width + px] || get(px, py) != target) continue;
    seen[(size_t)py * width + px] = 1;
    cells.push_back({px, py});
    todo.push_back({px + 1, py}), todo.push_back({px - 1, py}), todo.push_back({px, py + 1}), todo.push_back({px, py - 1});
  }
  return cells;
}

// A tool's icon, drawn in a square button.
static void toolIcon(ImDrawList* drawList, ImVec2 position, float size, int tool, ImU32 colour) {
  float thickness = std::max(1.0f, size * 0.09f), margin = size * 0.22f;
  ImVec2 topLeft(position.x + margin, position.y + margin), bottomRight(position.x + size - margin, position.y + size - margin);
  switch (tool) {
    case 0: {  // pencil: a slanted body and its tip
      ImVec2 points[4] = {{bottomRight.x - size * 0.12f, topLeft.y}, {bottomRight.x, topLeft.y + size * 0.12f}, {topLeft.x + size * 0.12f, bottomRight.y}, {topLeft.x, bottomRight.y - size * 0.12f}};
      drawList->AddConvexPolyFilled(points, 4, colour);
      drawList->AddTriangleFilled(ImVec2(topLeft.x, bottomRight.y - size * 0.12f), ImVec2(topLeft.x + size * 0.12f, bottomRight.y), ImVec2(topLeft.x - size * 0.04f, bottomRight.y + size * 0.04f), colour);
      break;
    }
    case 1: drawList->AddLine(ImVec2(topLeft.x, bottomRight.y), ImVec2(bottomRight.x, topLeft.y), colour, thickness * 1.4f); break;
    case 2: drawList->AddRect(topLeft, bottomRight, colour, 0, 0, thickness * 1.4f); break;
    case 3: drawList->AddRectFilled(topLeft, bottomRight, colour); break;
    case 4: drawList->AddEllipse(ImVec2((topLeft.x + bottomRight.x) / 2, (topLeft.y + bottomRight.y) / 2), ImVec2((bottomRight.x - topLeft.x) / 2, (bottomRight.y - topLeft.y) / 2.6f), colour, 0, 0, thickness * 1.4f); break;
    case 5: drawList->AddEllipseFilled(ImVec2((topLeft.x + bottomRight.x) / 2, (topLeft.y + bottomRight.y) / 2), ImVec2((bottomRight.x - topLeft.x) / 2, (bottomRight.y - topLeft.y) / 2.6f), colour); break;
    case 6: {  // fill: a tipped bucket and a drop
      ImVec2 points[4] = {{topLeft.x + size * 0.06f, topLeft.y + size * 0.22f}, {topLeft.x + size * 0.36f, topLeft.y - size * 0.04f}, {bottomRight.x - size * 0.04f, topLeft.y + size * 0.34f}, {topLeft.x + size * 0.3f, bottomRight.y - size * 0.02f}};
      drawList->AddConvexPolyFilled(points, 4, colour);
      drawList->AddCircleFilled(ImVec2(bottomRight.x - size * 0.02f, bottomRight.y - size * 0.06f), size * 0.09f, colour);
      break;
    }
  }
}

bool GfxEditor::toolBar() {
  static const char* names[] = {N_("Pencil (P)"), N_("Line (L)"), N_("Rectangle (R)"), N_("Filled rectangle (Shift+R)"),
                                N_("Ellipse (E)"), N_("Filled ellipse (Shift+E)"), N_("Fill (F)")};
  bool picked = false;
  float size = ImGui::GetFrameHeight();
  for (int i = 0; i < 7; i++) {
    if (i) ImGui::SameLine(0, 2);
    ImGui::PushID(i);
    bool on = (int)tool_ == i;
    ImVec2 position = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("tool", ImVec2(size, size))) tool_ = (Tool)i, picked = true;
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImU32 background = theme::col(on ? theme::Role::UiButtonActive : hovered ? theme::Role::UiButtonHover : theme::Role::UiButton);
    drawList->AddRectFilled(position, ImVec2(position.x + size, position.y + size), background, ImGui::GetStyle().FrameRounding);
    toolIcon(drawList, position, size, i, theme::col(theme::Role::UiText));
    if (hovered) ImGui::SetTooltip("%s", T(names[i]));
    ImGui::PopID();
  }
  // keys, while no text field has the keyboard
  ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) tool_ = Tool::Pen;
    if (ImGui::IsKeyPressed(ImGuiKey_L, false)) tool_ = Tool::Line;
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tool_ = io.KeyShift ? Tool::RectFilled : Tool::Rect;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) tool_ = io.KeyShift ? Tool::EllipseFilled : Tool::Ellipse;
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) tool_ = Tool::Fill;
  }
  return picked;
}

// ---------------------------------------------------------------------------
// Colours

bool GfxEditor::colourButton(const char* id, int colour, Tv tv, int* out, bool allowInherit, float size) {
  bool result = false;
  ImGui::PushID(id);
  uint32_t rgbColour = rgb(tv, colour < 0 ? 0 : colour);
  ImVec4 swatchColour((rgbColour >> 16 & 255) / 255.f, (rgbColour >> 8 & 255) / 255.f, (rgbColour & 255) / 255.f, colour < 0 ? 0.35f : 1.f);
  std::string tip = colour < 0 ? T("Same as the row above") : hexColour(colour);
  if (ImGui::ColorButton(tip.c_str(), swatchColour, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreview, ImVec2(size, size)))
    ImGui::OpenPopup("palette");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip.c_str());
  if (ImGui::BeginPopup("palette")) {
    ImGui::TextDisabled("%s", T("Hue down, luminance across"));
    for (int hue = 0; hue < 16; hue++) {
      for (int luminance = 0; luminance < 8; luminance++) {
        int value = hue << 4 | luminance << 1;
        uint32_t paletteRgb = rgb(tv, value);
        ImVec4 paletteColour((paletteRgb >> 16 & 255) / 255.f, (paletteRgb >> 8 & 255) / 255.f, (paletteRgb & 255) / 255.f, 1);
        if (luminance) ImGui::SameLine(0, 2);
        ImGui::PushID(value);
        bool selected = value == (colour & 0xFE);
        if (selected) ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2);
        if (ImGui::ColorButton(hexColour(value).c_str(), paletteColour, ImGuiColorEditFlags_NoBorder * !selected, ImVec2(em(22), em(16)))) {
          *out = value;
          result = true;
          ImGui::CloseCurrentPopup();
        }
        if (selected) ImGui::PopStyleVar();
        ImGui::PopID();
      }
    }
    if (allowInherit && ImGui::Button(T("Same as the row above"))) {
      *out = -1;
      result = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return result;
}

// ---------------------------------------------------------------------------
// The whole editor

void GfxEditor::render(Tv tv, const Profile& project) {
  kernelProfile_ = profile(project);
  ImGuiIO& io = ImGui::GetIO();
  bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
  if (focused && io.KeyCtrl && !ImGui::IsAnyItemActive()) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? redo() : undo();
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
  }
  int items = (int)(asset_.sprites.size() + asset_.playfields.size());
  item_ = std::clamp(item_, 0, std::max(0, items - 1));
  ImGui::SetCursorPos(ImVec2(8, 6));
  ImGui::BeginGroup();
  toolbar(project);
  ImGui::Separator();
  if (item_ < (int)asset_.sprites.size()) spriteView(asset_.sprites[item_], tv);
  else if (item_ - (int)asset_.sprites.size() < (int)asset_.playfields.size())
    playfieldView(asset_.playfields[item_ - asset_.sprites.size()], tv);
  ImGui::EndGroup();
  endFrame();
}

// SameLine if a widget of this label still fits on the line.
static void nextTo(const char* label, float spacing = -1) {
  float labelWidth = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
  float right = ImGui::GetItemRectMax().x + (spacing < 0 ? ImGui::GetStyle().ItemSpacing.x : spacing) + labelWidth;
  if (right < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine(0, spacing);
}

// The group a sprite or playfield goes to when the tables are split by group.
void GfxEditor::groupField(std::string& group) {
  if (asset_.split != Asset::Split::Group) return;
  std::set<std::string> known;
  for (auto& sprite : asset_.sprites) if (!sprite.group.empty()) known.insert(sprite.group);
  for (auto& playfield : asset_.playfields) if (!playfield.group.empty()) known.insert(playfield.group);
  float pick = ImGui::GetFrameHeight();
  ImGui::SetNextItemWidth(-pick - ImGui::GetStyle().ItemInnerSpacing.x);
  std::string edited = group;
  if (ImGui::InputTextWithHint("##group", T("group (none: the main file)"), &edited)) group = edited, changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Items of the same group share a generated file."));
  ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
  if (ImGui::BeginCombo("##groups", "", ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
    if (ImGui::Selectable(T("(none)"), group.empty())) group.clear(), changed();
    for (auto& existing : known)
      if (ImGui::Selectable(existing.c_str(), existing == group)) group = existing, changed();
    ImGui::EndCombo();
  }
}

void GfxEditor::toolbar(const Profile& project) {
  auto itemName = [&](int i) {
    if (i < (int)asset_.sprites.size()) return F("Sprite: {0}", {asset_.sprites[i].name});
    return F("Playfield: {0}", {asset_.playfields[i - asset_.sprites.size()].name});
  };
  int items = (int)(asset_.sprites.size() + asset_.playfields.size());
  ImGui::SetNextItemWidth(em(220));
  if (ImGui::BeginCombo("##item", items ? itemName(item_).c_str() : "")) {
    for (int i = 0; i < items; i++)
      if (ImGui::Selectable(itemName(i).c_str(), i == item_)) item_ = i, layer_ = frame_ = 0;
    ImGui::EndCombo();
  }
  nextTo(T("New sprite"));
  if (ImGui::Button(T("New sprite"))) {
    Sprite newSprite = blank().sprites[0];
    newSprite.name = "sprite" + std::to_string(asset_.sprites.size() + 1);
    asset_.sprites.push_back(newSprite);
    item_ = (int)asset_.sprites.size() - 1, layer_ = frame_ = 0;
    changed();
  }
  nextTo(T("New playfield"));
  if (ImGui::Button(T("New playfield"))) {
    Playfield newPlayfield;
    newPlayfield.name = "playfield" + std::to_string(asset_.playfields.size() + 1);
    asset_.playfields.push_back(newPlayfield);
    item_ = (int)(asset_.sprites.size() + asset_.playfields.size()) - 1;
    changed();
  }
  nextTo(T("Delete"));
  ImGui::BeginDisabled(items <= 1);
  if (ImGui::Button(T("Delete"))) {
    if (item_ < (int)asset_.sprites.size()) asset_.sprites.erase(asset_.sprites.begin() + item_);
    else asset_.playfields.erase(asset_.playfields.begin() + (item_ - asset_.sprites.size()));
    item_ = std::max(0, item_ - 1);
    changed();
  }
  ImGui::EndDisabled();
  // the kernel the graphics are for
  std::string kernelLabel = asset_.kernel.empty() ? F("Kernel: the project's ({0})", {project.describe()}) : F("Kernel: {0}", {kernelName(kernelProfile_.kernel)});
  nextTo(kernelLabel.c_str(), 24);
  ImGui::SetNextItemWidth(std::min(ImGui::CalcTextSize(kernelLabel.c_str()).x + 40, 360.0f));
  if (ImGui::BeginCombo("##kernel", kernelLabel.c_str())) {
    if (ImGui::Selectable(F("The project's ({0})", {project.describe()}).c_str(), asset_.kernel.empty())) asset_.kernel.clear(), changed();
    for (int i = 0; i < 5; i++)
      if (ImGui::Selectable(kernelName((Kernel)i), asset_.kernel == kernelId((Kernel)i)))
        asset_.kernel = kernelId((Kernel)i), changed();
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", F("What draws these graphics: {0}. The editor offers what it can show, and the warnings and the batari Basic copy follow it. The options (kernel_options, pfres) are read from the project's main program.",
                              {kernelProfile_.describe()}).c_str());
  // where the tables go: one file, or split (assembly and C; batari Basic copies its blocks)
  if (!kernelProfile_.basic()) {
    const char* splits[3] = {T("Tables: one file"), T("Tables: a file per item"), T("Tables: a file per group")};
    const char* currentSplit = splits[(int)asset_.split];
    nextTo(currentSplit, 24);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(currentSplit).x + em(40));
    if (ImGui::BeginCombo("##split", currentSplit)) {
      for (int i = 0; i < 3; i++)
        if (ImGui::Selectable(splits[i], (int)asset_.split == i)) asset_.split = (Asset::Split)i, changed();
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T("What saving writes for the program: all the tables in <name>.gfx.asm, each sprite and playfield in <name>.<item>.gfx.asm, or the items of each group together in <name>.<group>.gfx.asm (those with no group stay in <name>.gfx.asm). Files an earlier choice wrote are removed. C projects: .gfx.h."));
  }
  nextTo(T("Import"), 24);
  if (ImGui::Button(T("Import"))) ImGui::OpenPopup("import");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Reads the clipboard: assembly tables (VCSIDE, PlayerPal) or batari Basic blocks (player0:, playfield:...)."));
  nextTo(T("Copy as..."));
  if (ImGui::Button(T("Copy as..."))) ImGui::OpenPopup("copy");
  if (ImGui::BeginPopup("import")) {
    ImGui::TextUnformatted(T("Import what the clipboard holds as new sprites and playfields?"));
    ImGui::TextDisabled("%s", T("Assembly tables (VCSIDE, PlayerPal) or batari Basic blocks."));
    if (ImGui::Button(T("Import"))) {
      const char* clip = ImGui::GetClipboardText();
      Asset imported;
      std::string what;
      if (clip && importText(clip, &imported, &what, kernelProfile_)) {
        int at = (int)asset_.sprites.size();
        asset_.sprites.insert(asset_.sprites.end(), imported.sprites.begin(), imported.sprites.end());
        asset_.playfields.insert(asset_.playfields.end(), imported.playfields.begin(), imported.playfields.end());
        item_ = imported.sprites.empty() ? (int)(asset_.sprites.size() + asset_.playfields.size()) - 1 : at;
        layer_ = frame_ = 0;
        message_ = F("Imported: {0}.", {what});
        changed();
      } else {
        message_ = T("The clipboard holds no tables or blocks of graphics.");
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", T("Other editors (in the browser); their output can be imported here:"));
    ImGui::TextLinkOpenURL("PlayerPal", "https://alienbill.com/2600/playerpalnext.html");
    ImGui::SameLine();
    ImGui::TextLinkOpenURL("Atari Background Builder", "https://alienbill.com/2600/atari-background-builder/");
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("copy")) {
    bool sprite = item_ < (int)asset_.sprites.size();
    auto copy = [&](const std::string& text, const char* what) {
      ImGui::SetClipboardText(text.c_str());
      message_ = F("Copied as {0}.", {what});
    };
    if (ImGui::MenuItem(T("DASM tables"))) {
      copy(sprite ? exportDasm(asset_.sprites[item_]) : exportDasm(asset_.playfields[item_ - asset_.sprites.size()]), "DASM");
    }
    if (ImGui::MenuItem(T("C arrays"))) {
      copy(sprite ? exportC(asset_.sprites[item_]) : exportC(asset_.playfields[item_ - asset_.sprites.size()]), "C");
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T("For a C program (ELF cartridge). Animations use the types and functions of <file>.gfx.h."));
    if (ImGui::MenuItem(T("batari Basic"))) {
      copy(sprite ? exportBasic(asset_.sprites[item_], kernelProfile_) : exportBasic(asset_.playfields[item_ - asset_.sprites.size()], kernelProfile_), "batari Basic");
    }
    if (ImGui::MenuItem(T("batari Basic, kept up to date"))) {
      ImGui::SetClipboardText(basicRegion(fs::path(path_).filename().string(), asset_, item_, kernelProfile_).c_str());
      message_ = T("Copied: paste it in the program; every save of this file rewrites it.");
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T("The blocks between `rem vcsgfx` lines. Saving this file rewrites them in the project's programs (one region per animation frame)."));
    ImGui::Separator();
    ImGui::TextDisabled("%s", kernelProfile_.usesC ? T("On save, every item also goes to <file>.gfx.h for the program to include.")
                              : !kernelProfile_.basic() ? T("On save, every item also goes to <file>.gfx.asm for the program to include.")
                                                     : T("batari Basic: paste the blocks where the program sets its graphics."));
    ImGui::EndPopup();
  }
  if (!message_.empty()) {
    nextTo(message_.c_str(), 16);
    ImGui::TextDisabled("%s", message_.c_str());
  }
}

void GfxEditor::issues(const std::vector<Issue>& list) {
  if (list.empty()) {
    ImGui::TextDisabled("%s", T("Nothing to warn about."));
    return;
  }
  for (auto& issue : list) {
    ImVec4 colour = theme::colv(issue.severity == Issue::Error ? theme::Role::MsgError : issue.severity == Issue::Warning ? theme::Role::MsgWarning : theme::Role::MsgInfo);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::Bullet();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", issue.text.c_str());
  }
}

// ---------------------------------------------------------------------------
// Sprites

static void spriteBounds(const Sprite& sprite, int bounds[4], int margin) {
  bounds[0] = 0, bounds[1] = 0, bounds[2] = 8, bounds[3] = 1;
  bool first = true;
  for (auto& layer : sprite.layers) {
    int x0 = layer.dx, y0 = layer.dy, x1 = layer.dx + layer.width(), y1 = layer.dy + layer.height;
    if (first) bounds[0] = x0, bounds[1] = y0, bounds[2] = x1, bounds[3] = y1, first = false;
    bounds[0] = std::min(bounds[0], x0), bounds[1] = std::min(bounds[1], y0), bounds[2] = std::max(bounds[2], x1), bounds[3] = std::max(bounds[3], y1);
  }
  bounds[0] -= margin, bounds[1] -= margin, bounds[2] += margin, bounds[3] += margin;
}

// Draws the sprite as the TIA would: per pixel the first object in priority order
// (P0, M0, P1 and the players a kernel draws with it, M1, BL) that is on; a missile in the colour its player sets on that line.
void GfxEditor::drawSprite(const Sprite& sprite, int frame, int phase, ImVec2 origin, float cellWidth, float cellHeight, Tv tv, int backgroundColour, int highlight,
                           int bounds[4]) {
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  ImU32 backgroundRgb = col(tv, backgroundColour);
  drawList->AddRectFilled(origin, ImVec2(origin.x + (bounds[2] - bounds[0]) * cellWidth, origin.y + (bounds[3] - bounds[1]) * cellHeight), backgroundRgb);
  static const Obj order[] = {Obj::P0,  Obj::M0,  Obj::P1,  Obj::P2,  Obj::P3,  Obj::P4,  Obj::P5,
                              Obj::P6,  Obj::P7,  Obj::P8,  Obj::P9,  Obj::P10, Obj::P11, Obj::P12,
                              Obj::P13, Obj::P14, Obj::P15, Obj::P16, Obj::M1,  Obj::BL};
  for (int y = bounds[1]; y < bounds[3]; y++)
    for (int x = bounds[0]; x < bounds[2]; x++) {
      int winner = -1;
      for (Obj object : order) {
        for (int i = 0; i < (int)sprite.layers.size() && winner < 0; i++) {
          const Layer& layer = sprite.layers[i];
          if (layer.obj != object || (phase >= 0 && layer.phase != phase)) continue;
          if (layer.pixel(frame, y - layer.dy, x - layer.dx)) winner = i;
        }
        if (winner >= 0) break;
      }
      if (winner < 0) continue;
      const Layer& winnerLayer = sprite.layers[winner];
      // as the kernel shows it: one colour for an object that cannot change it per row
      auto colourAt = [&](const Layer& layer, int row) { return layer.colour(frame, kernelProfile_.rowColours(layer.obj) ? row : 0); };
      int colourIndex = colourAt(winnerLayer, y - winnerLayer.dy);
      if (winnerLayer.obj == Obj::M0 || winnerLayer.obj == Obj::M1)
        for (auto& playerLayer : sprite.layers)
          if (playerLayer.obj == (winnerLayer.obj == Obj::M0 ? Obj::P0 : Obj::P1) && playerLayer.phase == winnerLayer.phase && y - playerLayer.dy >= 0 && y - playerLayer.dy < playerLayer.height)
            colourIndex = colourAt(playerLayer, y - playerLayer.dy);
      ImU32 pixelRgb = col(tv, colourIndex);
      if (highlight >= 0 && winner != highlight && othersDim_) pixelRgb = mix(pixelRgb, backgroundRgb, 0.55f);
      ImVec2 cellOrigin(origin.x + (x - bounds[0]) * cellWidth, origin.y + (y - bounds[1]) * cellHeight);
      drawList->AddRectFilled(cellOrigin, ImVec2(cellOrigin.x + cellWidth, cellOrigin.y + cellHeight), pixelRgb);
    }
}

// The label of an object in the menus: "P0  player", "M1  missile"...
static std::string objLabel(Obj object) {
  return std::string(objName(object)) + "  " + (isPlayer(object) ? T("player") : object == Obj::BL ? T("ball") : T("missile"));
}
// The objects in the order the menus show them: the TIA's five, then the extra players.
static std::vector<Obj> objectList(const Profile& profile) {
  std::vector<Obj> objects = {Obj::P0, Obj::P1, Obj::M0, Obj::M1, Obj::BL};
  for (int number = 2; number < profile.players(); number++) objects.push_back(playerObj(number));
  return objects;
}

void GfxEditor::spriteView(Sprite& sprite, Tv tv) {
  layer_ = std::clamp(layer_, 0, std::max(0, (int)sprite.layers.size() - 1));
  frame_ = std::clamp(frame_, 0, sprite.frames - 1);
  float contentHeight = ImGui::GetContentRegionAvail().y;
  // narrow: the preview and the warnings go under the layers instead of a column of their own
  bool narrow = ImGui::GetContentRegionAvail().x < em(780);
  float side = em(narrow ? 230 : 250);
  auto preview = [&] {
    ImGui::SeparatorText(T("Preview"));
    ImGui::Checkbox(T("flicker"), &flicker_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Off: every phase at once, as if there were no flicker."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(70));
    ImGui::SliderInt("##scale", &previewScale_, 1, 4, "x%d");
    int tick = (int)(ImGui::GetTime() * 60);
    int previewFrame = (tick / std::max(1, sprite.frameTicks)) % sprite.frames;
    if (anim_ >= 0 && anim_ < (int)sprite.animations.size() && !sprite.animations[anim_].steps.empty())
      previewFrame = sprite.frameAt(sprite.animations[anim_], (int)((ImGui::GetTime() - animStart_) * 60));
    int phase = flicker_ && sprite.phases() > 1 ? tick % sprite.phases() : -1;
    int previewBounds[4];
    spriteBounds(sprite, previewBounds, 1);
    float previewCellWidth = (float)previewScale_, previewCellHeight = previewScale_ * kLineAspect * sprite.lineHeight;
    ImVec2 previewOrigin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2((previewBounds[2] - previewBounds[0]) * previewCellWidth, (previewBounds[3] - previewBounds[1]) * previewCellHeight));
    drawSprite(sprite, previewFrame, phase, previewOrigin, previewCellWidth, previewCellHeight, tv, sprite.background, -1, previewBounds);
    ImGui::SeparatorText(T("Warnings"));
    issues(check(sprite, kernelProfile_));
  };

  // --- left: the sprite, its layers and the selected layer
  ImGui::BeginChild("layers", ImVec2(side, contentHeight), ImGuiChildFlags_Borders);
  std::string name = sprite.name;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) || (ImGui::IsItemDeactivatedAfterEdit())) {
    if (!name.empty()) sprite.name = name, changed();
  }
  groupField(sprite.group);
  int linesPerRow = sprite.lineHeight;
  ImGui::SetNextItemWidth(em(110));
  if (ImGui::SliderInt(T("lines per row"), &linesPerRow, 1, 4)) sprite.lineHeight = linesPerRow, changed();
  int ticks = sprite.frameTicks;
  ImGui::SetNextItemWidth(em(110));
  if (ImGui::SliderInt(T("frames per step"), &ticks, 1, 30)) sprite.frameTicks = ticks, changed();
  ImGui::SeparatorText(T("Layers"));
  static const theme::Role objRole[] = {theme::Role::GfxP0, theme::Role::GfxP1, theme::Role::GfxM0, theme::Role::GfxM1, theme::Role::GfxBL};
  auto objCol = [&](Obj object) { return theme::colv((int)object < 5 ? objRole[(int)object] : theme::Role::GfxP1); };  // extra players: P1's family
  for (int i = 0; i < (int)sprite.layers.size(); i++) {
    auto& layer = sprite.layers[i];
    ImGui::PushID(i);
    ImGui::PushStyleColor(ImGuiCol_Text, objCol(layer.obj));
    ImGui::TextUnformatted(objName(layer.obj));
    ImGui::PopStyleColor();
    ImGui::SameLine(36);
    std::string label = (layer.name.empty() ? std::string(T("(no name)")) : layer.name) + "##l";
    if (ImGui::Selectable(label.c_str(), i == layer_, 0, ImVec2(side - 110, 0))) layer_ = i;
    ImGui::SameLine(side - 62);
    ImGui::TextDisabled("%s", F("phase {0}", {std::to_string(layer.phase + 1)}).c_str());
    ImGui::PopID();
  }
  if (ImGui::Button(T("Add"))) ImGui::OpenPopup("add");
  if (ImGui::BeginPopup("add")) {
    for (Obj object : objectList(kernelProfile_)) {
      bool used = std::any_of(sprite.layers.begin(), sprite.layers.end(), [&](const Layer& layer) { return layer.obj == object && layer.phase == 0; });
      std::string why = kernelProfile_.missing(object);
      if (ImGui::MenuItem(objLabel(object).c_str(), used ? T("used in phase 1") : "", false, why.empty()) && why.empty()) {
        Layer layer;
        layer.obj = object;
        int hgt = sprite.layers.empty() ? 8 : sprite.layers[layer_].height;
        layer.resize(sprite.frames, hgt);
        if (!sprite.layers.empty()) layer.dx = sprite.layers[layer_].dx + sprite.layers[layer_].width() + 1, layer.dy = sprite.layers[layer_].dy;
        // the first phase where the object is free
        for (int candidatePhase = 0; candidatePhase < 4; candidatePhase++)
          if (std::none_of(sprite.layers.begin(), sprite.layers.end(), [&](const Layer& existing) { return existing.obj == object && existing.phase == candidatePhase; })) {
            layer.phase = candidatePhase;
            break;
          }
        layer.colours.assign(sprite.frames, std::vector<int>(layer.height, -1));
        for (auto& frameColours : layer.colours) frameColours[0] = object == Obj::BL ? 0x0E : 0x46;
        sprite.layers.push_back(layer);
        layer_ = (int)sprite.layers.size() - 1;
        changed();
      }
      if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(sprite.layers.size() <= 1);
  if (ImGui::Button(T("Remove"))) {
    sprite.layers.erase(sprite.layers.begin() + layer_);
    layer_ = std::max(0, layer_ - 1);
    changed();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::ArrowButton("up", ImGuiDir_Up) && layer_ > 0) std::swap(sprite.layers[layer_], sprite.layers[layer_ - 1]), layer_--, changed();
  ImGui::SameLine();
  if (ImGui::ArrowButton("down", ImGuiDir_Down) && layer_ + 1 < (int)sprite.layers.size())
    std::swap(sprite.layers[layer_], sprite.layers[layer_ + 1]), layer_++, changed();

  if (!sprite.layers.empty()) {
    Layer& layer = sprite.layers[layer_];
    ImGui::SeparatorText(T("Layer"));
    Obj chosen = layer.obj;
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::BeginCombo(T("object"), objName(layer.obj))) {
      for (Obj object : objectList(kernelProfile_)) {
        std::string why = kernelProfile_.missing(object);
        if (ImGui::Selectable(objLabel(object).c_str(), object == layer.obj, why.empty() ? 0 : ImGuiSelectableFlags_Disabled)) chosen = object;
        if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
      }
      ImGui::EndCombo();
    }
    if (chosen != layer.obj) {
      bool wasPlayer = isPlayer(layer.obj);
      layer.obj = chosen;
      if (wasPlayer != isPlayer(layer.obj)) {
        layer.size = 1;
        for (auto& frameBits : layer.bits)
          for (auto& rowByte : frameBits) rowByte = rowByte ? (isPlayer(layer.obj) ? 0x80 : 1) : 0;
      }
      changed();
    }
    std::string layerName = layer.name;
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::InputText(T("name"), &layerName)) layer.name = layerName, changed();
    int phaseChoice = layer.phase;
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::Combo(T("phase"), &phaseChoice, "1\0" "2\0" "3\0" "4\0")) layer.phase = phaseChoice, changed();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T("Layers in different phases are drawn on alternate frames (flicker), so one object can be used more than once."));
    int pos[2] = {layer.dx, layer.dy};
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::DragInt2(T("position"), pos, 0.2f, -64, 64)) layer.dx = pos[0], layer.dy = pos[1], changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Colour clocks and rows. Shift+drag on the canvas moves the layer."));
    if (isPlayer(layer.obj)) {
      int sizeIndex = layer.size == 4 ? 2 : layer.size == 2 ? 1 : 0;
      ImGui::SetNextItemWidth(em(120));
      if (ImGui::Combo(T("width"), &sizeIndex, "1x\0" "2x\0" "4x\0")) layer.size = 1 << sizeIndex, changed();
      bool reflected = layer.reflect;
      if (ImGui::Checkbox(T("reflected (REFP)"), &reflected)) layer.reflect = reflected, changed();
    } else {
      int sizeIndex = layer.size >= 8 ? 3 : layer.size >= 4 ? 2 : layer.size >= 2 ? 1 : 0;
      ImGui::SetNextItemWidth(em(120));
      if (ImGui::Combo(T("width"), &sizeIndex, "1\0" "2\0" "4\0" "8\0")) layer.size = 1 << sizeIndex, changed();
    }
    int rowCount = layer.height;
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::DragInt(T("rows"), &rowCount, 0.2f, 1, 128)) layer.resize(sprite.frames, rowCount), changed();
  }
  if (narrow) preview();
  ImGui::EndChild();
  ImGui::SameLine();

  // --- centre: the canvas, the row colours of the layer and the frames
  float rightW = narrow ? 0 : em(270);
  float centreW = std::max(160.0f, ImGui::GetContentRegionAvail().x - rightW - (narrow ? 0 : 8));
  ImGui::BeginChild("canvas", ImVec2(centreW, contentHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  toolBar();
  ImGui::SetNextItemWidth(em(120));
  ImGui::SliderFloat(T("zoom"), &zoom_, 6, 32, "%.0f");
  ImGui::SameLine();
  int newBackground;
  if (colourButton("bk", sprite.background, tv, &newBackground, false, ImGui::GetFrameHeight())) sprite.background = newBackground, changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Only to see the sprite against it: the program sets its own COLUBK."));
  ImGui::SameLine();
  ImGui::TextUnformatted(T("background"));
  ImGui::SameLine();
  ImGui::Checkbox(T("dim the other layers"), &othersDim_);
  int bounds[4];
  spriteBounds(sprite, bounds, 2);
  float cellWidth = zoom_, cellHeight = zoom_ * kLineAspect * sprite.lineHeight;
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 size((bounds[2] - bounds[0]) * cellWidth, (bounds[3] - bounds[1]) * cellHeight);
  ImGui::InvisibleButton("grid", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  drawSprite(sprite, frame_, -1, origin, cellWidth, cellHeight, tv, sprite.background, layer_, bounds);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  for (int x = bounds[0]; x <= bounds[2]; x++)
    drawList->AddLine(ImVec2(origin.x + (x - bounds[0]) * cellWidth, origin.y), ImVec2(origin.x + (x - bounds[0]) * cellWidth, origin.y + size.y), theme::col(x % 8 == 0 ? theme::Role::GfxGridMajor : theme::Role::GfxGrid));
  for (int y = bounds[1]; y <= bounds[3]; y++)
    drawList->AddLine(ImVec2(origin.x, origin.y + (y - bounds[1]) * cellHeight), ImVec2(origin.x + size.x, origin.y + (y - bounds[1]) * cellHeight), theme::col(theme::Role::GfxGrid));
  if (!sprite.layers.empty()) {
    Layer& layer = sprite.layers[layer_];
    ImVec2 layerOrigin(origin.x + (layer.dx - bounds[0]) * cellWidth, origin.y + (layer.dy - bounds[1]) * cellHeight);
    drawList->AddRect(layerOrigin, ImVec2(layerOrigin.x + layer.width() * cellWidth, layerOrigin.y + layer.height * cellHeight), theme::col(theme::Role::GfxOutline), 0, 0, 1.5f);
    // painting: the pencil sets or clears what was under the first click, the other
    // tools draw with the left button and erase with the right; Shift+drag moves
    ImVec2 mouse = ImGui::GetMousePos();
    int mx = bounds[0] + (int)std::floor((mouse.x - origin.x) / cellWidth), my = bounds[1] + (int)std::floor((mouse.y - origin.y) / cellHeight);
    int lx = mx - layer.dx, ly = my - layer.dy;
    auto setPx = [&](int x, int y, bool on) {
      if (x < 0 || x >= layer.width() || y < 0 || y >= layer.height) return;
      uint8_t& rowBits = layer.bits[frame_][y];
      uint8_t newRowBits = rowBits;
      if (isPlayer(layer.obj)) {
        int bit = x / layer.size;
        if (layer.reflect) bit = 7 - bit;
        newRowBits = on ? rowBits | (0x80 >> bit) : rowBits & ~(0x80 >> bit);
      } else {
        newRowBits = on ? 1 : 0;
      }
      if (newRowBits != rowBits) rowBits = newRowBits, changed();
    };
    bool pressed = hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right));
    bool right = ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (pressed) {
      if (ImGui::GetIO().KeyShift && tool_ == Tool::Pen) {
        moving_ = true;
        moveDx_ = mx - layer.dx, moveDy_ = my - layer.dy;
      } else if (tool_ == Tool::Pen) {
        paintValue_ = right ? 0 : !layer.pixel(frame_, ly, lx);
      } else if (tool_ == Tool::Fill) {
        bool on = !right && !layer.pixel(frame_, ly, lx);
        for (auto [x, y] : fillCells(lx, ly, layer.width(), layer.height, [&](int x, int y) { return layer.pixel(frame_, y, x); }))
          setPx(x, y, on);
      } else {
        shapeX_ = lx, shapeY_ = ly, paintValue_ = right ? 0 : 1;
      }
    }
    auto shape = [&]() -> Cells {
      switch (tool_) {
        case Tool::Line: return lineCells(shapeX_, shapeY_, lx, ly);
        case Tool::Rect: return rectCells(shapeX_, shapeY_, lx, ly, false);
        case Tool::RectFilled: return rectCells(shapeX_, shapeY_, lx, ly, true);
        case Tool::Ellipse: return ellipseCells(shapeX_, shapeY_, lx, ly, false);
        case Tool::EllipseFilled: return ellipseCells(shapeX_, shapeY_, lx, ly, true);
        default: return {};
      }
    };
    bool shapeTool = tool_ != Tool::Pen && tool_ != Tool::Fill;
    if (active && moving_) {
      if (layer.dx != mx - moveDx_ || layer.dy != my - moveDy_) layer.dx = mx - moveDx_, layer.dy = my - moveDy_, changed();
    } else if (active && paintValue_ >= 0 && tool_ == Tool::Pen) {
      setPx(lx, ly, paintValue_);
    } else if (active && paintValue_ >= 0 && shapeTool) {
      // the shape as it would be, over the canvas, until the button is let go
      ImU32 shapeColour = theme::col(paintValue_ ? theme::Role::GfxOutline : theme::Role::GfxLockedHatch);
      for (auto [x, y] : shape())
        if (x >= 0 && x < layer.width() && y >= 0 && y < layer.height) {
          ImVec2 cellPosition(origin.x + (layer.dx + x - bounds[0]) * cellWidth, origin.y + (layer.dy + y - bounds[1]) * cellHeight);
          drawList->AddRectFilled(ImVec2(cellPosition.x + 1, cellPosition.y + 1), ImVec2(cellPosition.x + cellWidth - 1, cellPosition.y + cellHeight - 1), shapeColour);
        }
    }
    if (paintValue_ >= 0 && shapeTool && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      for (auto [x, y] : shape()) setPx(x, y, paintValue_);
      paintValue_ = -1;
    }
    if (!active && !(paintValue_ >= 0 && shapeTool)) paintValue_ = -1, moving_ = false;
    if (hovered && !active)
      ImGui::SetTooltip("x %d, %s %d", mx - layer.dx, T("row"), my - layer.dy + 1);

    // the row colours of the layer, next to its rows
    ImVec2 end = ImGui::GetCursorScreenPos();
    float swatchSize = std::min(cellHeight - 2, 18.0f);
    int colourRows = kernelProfile_.rowColours(layer.obj) ? layer.height : 1;  // otherwise one colour for the object
    for (int row = 0; row < colourRows; row++) {
      ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x + 8, origin.y + (layer.dy - bounds[1] + row) * cellHeight + (cellHeight - swatchSize) / 2));
      int currentColour = layer.colours[frame_][row], newColour;
      std::string id = "c" + std::to_string(row);
      if (colourButton(id.c_str(), row == 0 && currentColour < 0 ? layer.colour(frame_, 0) : currentColour, tv, &newColour, row > 0, swatchSize)) {
        if (ImGui::GetIO().KeyAlt) {
          for (int frameIndex = 0; frameIndex < sprite.frames; frameIndex++) layer.colours[frameIndex][row] = newColour;  // Alt: every frame
        } else {
          layer.colours[frame_][row] = newColour;
        }
        changed();
      }
    }
    if (colourRows == 1 && layer.height > 1 && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", F("{0}: one colour for the whole object in this kernel.", {objName(layer.obj)}).c_str());
    ImGui::SetCursorScreenPos(end);
  }
  ImGui::Dummy(ImVec2(0, 6));
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", T("Left: draw · Right: erase · Shift+drag with the pencil: move the layer · Alt+colour: every frame"));
  ImGui::PopStyleColor();

  // frames
  ImGui::SeparatorText(T("Frames"));
  int thumbBounds[4];
  spriteBounds(sprite, thumbBounds, 0);
  float thumbCellWidth = 3, thumbCellHeight = 3 * kLineAspect * sprite.lineHeight;
  for (int frameIndex = 0; frameIndex < sprite.frames; frameIndex++) {
    ImGui::PushID(frameIndex);
    if (frameIndex) ImGui::SameLine();
    ImVec2 thumbOrigin = ImGui::GetCursorScreenPos();
    ImVec2 thumbSize((thumbBounds[2] - thumbBounds[0]) * thumbCellWidth + 6, (thumbBounds[3] - thumbBounds[1]) * thumbCellHeight + 6);
    if (ImGui::InvisibleButton("f", thumbSize)) frame_ = frameIndex;
    drawSprite(sprite, frameIndex, -1, ImVec2(thumbOrigin.x + 3, thumbOrigin.y + 3), thumbCellWidth, thumbCellHeight, tv, sprite.background, -1, thumbBounds);
    drawList->AddRect(thumbOrigin, ImVec2(thumbOrigin.x + thumbSize.x, thumbOrigin.y + thumbSize.y), theme::col(frameIndex == frame_ ? theme::Role::GfxThumbSelected : theme::Role::GfxThumbFrame), 3, 0,
                frameIndex == frame_ ? 2.0f : 1.0f);
    ImGui::PopID();
  }
  if (ImGui::Button(T("New frame"))) sprite.insertFrame(frame_ + 1, -1), frame_++, changed();
  ImGui::SameLine();
  if (ImGui::Button(T("Duplicate"))) sprite.insertFrame(frame_ + 1, frame_), frame_++, changed();
  ImGui::SameLine();
  ImGui::BeginDisabled(sprite.frames <= 1);
  if (ImGui::Button(T("Delete frame"))) sprite.removeFrame(frame_), frame_ = std::min(frame_, sprite.frames - 1), changed();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::ArrowButton("fl", ImGuiDir_Left) && frame_ > 0) sprite.swapFrames(frame_, frame_ - 1), frame_--, changed();
  ImGui::SameLine();
  if (ImGui::ArrowButton("fr", ImGuiDir_Right) && frame_ + 1 < sprite.frames) sprite.swapFrames(frame_, frame_ + 1), frame_++, changed();
  animations(sprite, tv, thumbBounds, thumbCellWidth, thumbCellHeight);
  ImGui::EndChild();
  if (!narrow) {
    ImGui::SameLine();
    ImGui::BeginChild("preview", ImVec2(rightW, contentHeight), ImGuiChildFlags_Borders);
    preview();
    ImGui::EndChild();
  }
}

// Animations: named sequences of steps, each a frame shown for some video frames.
void GfxEditor::animations(Sprite& sprite, Tv tv, int thumbBounds[4], float thumbCellWidth, float thumbCellHeight) {
  ImGui::SeparatorText(T("Animations"));
  anim_ = std::clamp(anim_, 0, std::max(0, (int)sprite.animations.size() - 1));
  if (!sprite.animations.empty()) {
    ImGui::SetNextItemWidth(em(150));
    if (ImGui::BeginCombo("##anim", sprite.animations[anim_].name.c_str())) {
      for (int i = 0; i < (int)sprite.animations.size(); i++)
        if (ImGui::Selectable((sprite.animations[i].name + "##" + std::to_string(i)).c_str(), i == anim_)) anim_ = i, step_ = 0, animStart_ = ImGui::GetTime();
      ImGui::EndCombo();
    }
    ImGui::SameLine();
  }
  if (ImGui::Button(T("New animation"))) {
    Animation animation;
    animation.name = sprite.animations.empty() ? "walk" : "anim" + std::to_string(sprite.animations.size() + 1);
    for (int frameIndex = 0; frameIndex < sprite.frames; frameIndex++) animation.steps.push_back({frameIndex, sprite.frameTicks});
    sprite.animations.push_back(animation);
    anim_ = (int)sprite.animations.size() - 1, step_ = 0, animStart_ = ImGui::GetTime();
    changed();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Starts with every frame in order; add, remove and reorder steps below."));
  if (sprite.animations.empty()) {
    ImGui::TextDisabled("%s", T("No animations: the preview shows the frames in order."));
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button(T("Delete animation"))) {
    sprite.animations.erase(sprite.animations.begin() + anim_);
    anim_ = std::max(0, anim_ - 1);
    changed();
    return;
  }
  Animation& animation = sprite.animations[anim_];
  std::string name = animation.name;
  ImGui::SetNextItemWidth(em(150));
  if (ImGui::InputText((std::string(T("name")) + "##anim").c_str(), &name) && !name.empty()) animation.name = name, changed();
  ImGui::SameLine();
  if (ImGui::Checkbox(T("loops"), &animation.loop)) changed(), animStart_ = ImGui::GetTime();
  char seconds[16];
  std::snprintf(seconds, sizeof seconds, "%.2f", animation.length() / 60.0);
  std::string length = F("{0} video frames ({1} s on NTSC)", {std::to_string(animation.length()), seconds});
  nextTo(length.c_str());
  ImGui::TextDisabled("%s", length.c_str());
  // the steps: the frame each shows and for how long
  step_ = std::clamp(step_, 0, std::max(0, (int)animation.steps.size() - 1));
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  float thumbW = std::max(30.0f, (thumbBounds[2] - thumbBounds[0]) * thumbCellWidth + 6);
  for (int i = 0; i < (int)animation.steps.size(); i++) {
    AnimStep& step = animation.steps[i];
    ImGui::PushID(i + 1000);
    if (i && ImGui::GetItemRectMax().x + thumbW + 8 < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
    ImGui::BeginGroup();
    ImVec2 thumbOrigin = ImGui::GetCursorScreenPos();
    ImVec2 thumbSize(thumbW, (thumbBounds[3] - thumbBounds[1]) * thumbCellHeight + 6);
    if (ImGui::InvisibleButton("s", thumbSize)) step_ = i, frame_ = std::clamp(step.frame, 0, sprite.frames - 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", F("Step {0}: frame {1}", {std::to_string(i + 1), std::to_string(step.frame + 1)}).c_str());
    drawSprite(sprite, std::clamp(step.frame, 0, sprite.frames - 1), -1, ImVec2(thumbOrigin.x + 3, thumbOrigin.y + 3), thumbCellWidth, thumbCellHeight, tv, sprite.background, -1, thumbBounds);
    drawList->AddRect(thumbOrigin, ImVec2(thumbOrigin.x + thumbSize.x, thumbOrigin.y + thumbSize.y), theme::col(i == step_ ? theme::Role::GfxStepSelected : theme::Role::GfxThumbFrame), 3, 0,
                i == step_ ? 2.0f : 1.0f);
    int stepTicks = step.ticks;
    ImGui::SetNextItemWidth(thumbW);
    if (ImGui::DragInt("##t", &stepTicks, 0.2f, 1, 255)) step.ticks = stepTicks, changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Video frames this step lasts (60 a second on NTSC, 50 on PAL)"));
    ImGui::EndGroup();
    ImGui::PopID();
  }
  if (ImGui::Button(F("Add frame {0}", {std::to_string(frame_ + 1)}).c_str())) {
    int ticks = animation.steps.empty() ? sprite.frameTicks : animation.steps[step_].ticks;
    animation.steps.insert(animation.steps.begin() + (animation.steps.empty() ? 0 : step_ + 1), AnimStep{frame_, ticks});
    step_ = animation.steps.empty() ? 0 : step_ + 1;
    changed();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("A step with the frame being edited, after the selected step"));
  ImGui::SameLine();
  ImGui::BeginDisabled(animation.steps.empty());
  if (ImGui::Button(T("Remove step"))) animation.steps.erase(animation.steps.begin() + step_), step_ = std::max(0, step_ - 1), changed();
  ImGui::SameLine();
  if (ImGui::ArrowButton("sl", ImGuiDir_Left) && step_ > 0) std::swap(animation.steps[step_], animation.steps[step_ - 1]), step_--, changed();
  ImGui::SameLine();
  if (ImGui::ArrowButton("sr", ImGuiDir_Right) && step_ + 1 < (int)animation.steps.size()) std::swap(animation.steps[step_], animation.steps[step_ + 1]), step_++, changed();
  ImGui::EndDisabled();
  if (kernelProfile_.basic()) {
    ImGui::TextUnformatted(T("batari Basic variables:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(60));
    if (ImGui::InputText(T("counter"), &animation.basicTimer)) changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Two variables the program does not use for anything else (a-z or ones it has dimmed)."));
    nextTo((std::string("12345678") + T("step")).c_str());
    ImGui::SetNextItemWidth(em(60));
    if (ImGui::InputText(T("step"), &animation.basicStep)) changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Two variables the program does not use for anything else (a-z or ones it has dimmed)."));
  }
}

// ---------------------------------------------------------------------------
// Playfields

void GfxEditor::playfieldView(Playfield& playfield, Tv tv) {
  // the columns this kernel shows (a multisprite row: the left half, mirrored)
  int firstCol = kernelProfile_.pfFirst(), lastCol = 39 - firstCol;
  bool mirrorOnly = kernelProfile_.pfMirrorOnly();
  // what the kernel shows: its columns (mirrored if it can only mirror) and the colours it can change
  auto shown = [&](int row, int column) {
    if (mirrorOnly && column >= 20) column = 39 - column;
    return column >= firstCol && column <= lastCol && playfield.pixel(row, column);
  };
  bool pfRows = kernelProfile_.pfRowColours(), bkRows = kernelProfile_.bkRowColours();
  auto pfShown = [&](int row) { return playfield.pfAt(pfRows ? row : 0); };
  auto bkShown = [&](int row) { return playfield.bkAt(bkRows ? row : 0); };
  float contentHeight = ImGui::GetContentRegionAvail().y;
  bool narrow = ImGui::GetContentRegionAvail().x < 1000;
  auto preview = [&](float width) {
    ImGui::SeparatorText(T("Preview"));
    ImVec2 previewOrigin = ImGui::GetCursorScreenPos();
    float previewCellWidth = (width - 20) / 40, previewCellHeight = std::max(1.0f, previewCellWidth / 4 * kLineAspect * playfield.lineHeight);
    ImGui::Dummy(ImVec2(40 * previewCellWidth, playfield.rows.size() * previewCellHeight));
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int row = 0; row < (int)playfield.rows.size(); row++) {
      float y = previewOrigin.y + row * previewCellHeight;
      drawList->AddRectFilled(ImVec2(previewOrigin.x, y), ImVec2(previewOrigin.x + 40 * previewCellWidth, y + previewCellHeight), col(tv, bkShown(row)));
      for (int column = 0; column < 40; column++)
        if (shown(row, column)) drawList->AddRectFilled(ImVec2(previewOrigin.x + column * previewCellWidth, y), ImVec2(previewOrigin.x + (column + 1) * previewCellWidth, y + previewCellHeight), col(tv, pfShown(row)));
    }
    ImGui::SeparatorText(T("Warnings"));
    issues(check(playfield, kernelProfile_));
  };
  auto props = [&] {
    std::string name = playfield.name;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##name", &name) && !name.empty()) playfield.name = name, changed();
    groupField(playfield.group);
    int rows = (int)playfield.rows.size();
    ImGui::SetNextItemWidth(em(110));
    if (ImGui::DragInt(T("rows"), &rows, 0.2f, 1, kernelProfile_.pfRowsMax())) {
      PfRow last = playfield.rows.back();
      last.pf = last.bk = -1;
      playfield.rows.resize(rows, last);
      changed();
    }
    if (kernelProfile_.pfRowsDefault()) {
      ImGui::SameLine();
      if (ImGui::SmallButton(std::to_string(kernelProfile_.pfRowsDefault()).c_str())) {
        PfRow last = playfield.rows.back();
        last.pf = last.bk = -1;
        playfield.rows.resize(kernelProfile_.pfRowsDefault(), last);
        changed();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The rows the program shows"));
    }
    int linesPerRow = playfield.lineHeight;
    ImGui::SetNextItemWidth(em(110));
    if (ImGui::DragInt(T("lines per row"), &linesPerRow, 0.2f, 1, 64)) playfield.lineHeight = linesPerRow, changed();
    int newColour;
    if (colourButton("pf", playfield.pfColour, tv, &newColour, false, ImGui::GetFrameHeight())) playfield.pfColour = newColour, changed();
    ImGui::SameLine();
    ImGui::TextUnformatted(T("playfield (COLUPF)"));
    if (colourButton("bk", playfield.bkColour, tv, &newColour, false, ImGui::GetFrameHeight())) playfield.bkColour = newColour, changed();
    ImGui::SameLine();
    ImGui::TextUnformatted(T("background (COLUBK)"));
    if (mirrorOnly) {
      ImGui::Dummy(ImVec2(0, 8));
      if (ImGui::Button(T("Mirror every row"))) {
        for (int rowIndex = 0; rowIndex < (int)playfield.rows.size(); rowIndex++) playfield.setAsymmetric(rowIndex, false), playfield.rows[rowIndex].mode = PfMode::Mirror;
        changed();
      }
      ImGui::Dummy(ImVec2(0, 8));
      ImGui::TextWrapped("%s", T("This kernel draws the left half from column 5 and mirrors it: draw on the left, the right half follows."));
      return;
    }
    ImGui::SeparatorText(T("All rows"));
    if (ImGui::Button(T("Mirror"))) {
      for (int rowIndex = 0; rowIndex < (int)playfield.rows.size(); rowIndex++)
        if (!playfield.rows[rowIndex].asymmetric) playfield.rows[rowIndex].mode = PfMode::Mirror;
      changed();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Repeat"))) {
      for (int rowIndex = 0; rowIndex < (int)playfield.rows.size(); rowIndex++)
        if (!playfield.rows[rowIndex].asymmetric) playfield.rows[rowIndex].mode = PfMode::Repeat;
      changed();
    }
    if (ImGui::Button(T("Symmetric (drop the right halves)"))) {
      for (int rowIndex = 0; rowIndex < (int)playfield.rows.size(); rowIndex++) playfield.setAsymmetric(rowIndex, false);
      changed();
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextWrapped("%s", T("Each row has its own mode (M mirror, R repeat), an A when its right half is drawn on its own, and its colours. Drawing on the right half of a symmetric row makes it asymmetric."));
  };
  auto canvas = [&] {
    ImGui::SetNextItemWidth(em(120));
    ImGui::SliderFloat(T("zoom"), &zoom_, 6, 32, "%.0f");
    ImGui::SameLine();
    int newBackground;
    if (colourButton("bkc", playfield.bkColour, tv, &newBackground, false, ImGui::GetFrameHeight())) playfield.bkColour = newBackground, changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("COLUBK of the rows that do not set their own"));
    ImGui::SameLine();
    ImGui::TextUnformatted(T("background (COLUBK)"));
    // the 40 columns fit the space left by the row numbers and the row buttons; the zoom is the most
    float rowButtons = 4 * (ImGui::GetFrameHeight() + 8) + 16;
    float cellWidth = std::max(9.0f, std::min(zoom_ * 1.2f, (ImGui::GetContentRegionAvail().x - 28 - rowButtons) / 40));
    // rows keep the TV's proportion unless that leaves no room for their buttons (the preview keeps it always)
    float cellHeight = std::max(ImGui::GetFrameHeight() + 2, cellWidth / 4 * kLineAspect * playfield.lineHeight);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // register names over the columns
    ImVec2 top = ImGui::GetCursorScreenPos();
    bool shortNames = 4 * cellWidth < ImGui::CalcTextSize("PF0").x + 4;
    const char* registerNames[] = {shortNames ? "0" : "PF0", shortNames ? "1" : "PF1", shortNames ? "2" : "PF2"};
    int starts[] = {0, 4, 12, 20};
    for (int i = 0; i < 3; i++) {
      drawList->AddText(ImVec2(top.x + 28 + starts[i] * cellWidth + 2, top.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), registerNames[i]);
      drawList->AddText(ImVec2(top.x + 28 + (20 + starts[i]) * cellWidth + 2, top.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), registerNames[i]);
    }
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() + 2));
    ImVec2 origin = ImGui::GetCursorScreenPos();
    origin.x += 28;  // row numbers
    ImVec2 size(40 * cellWidth, playfield.rows.size() * cellHeight);
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("grid", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    for (int rowIndex = 0; rowIndex < (int)playfield.rows.size(); rowIndex++) {
      float y = origin.y + rowIndex * cellHeight;
      drawList->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y + cellHeight), col(tv, bkShown(rowIndex)));
      ImU32 ink = col(tv, pfShown(rowIndex));
      for (int column = 0; column < 40; column++)
        if (playfield.pixel(rowIndex, column)) {
          ImU32 cellColour = column >= 20 && !playfield.rows[rowIndex].asymmetric ? mix(ink, col(tv, bkShown(rowIndex)), 0.25f) : ink;  // a copy of the left half
          drawList->AddRectFilled(ImVec2(origin.x + column * cellWidth, y), ImVec2(origin.x + (column + 1) * cellWidth, y + cellHeight), cellColour);
        }
      char rowNumber[8];
      std::snprintf(rowNumber, sizeof rowNumber, "%d", rowIndex + 1);
      drawList->AddText(ImVec2(origin.x - 26, y + (cellHeight - ImGui::GetTextLineHeight()) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), rowNumber);
    }
    for (int column = 0; column <= 40; column++) {
      bool isRegisterBoundary = column == 4 || column == 12 || column == 24 || column == 32, half = column == 20;
      drawList->AddLine(ImVec2(origin.x + column * cellWidth, origin.y), ImVec2(origin.x + column * cellWidth, origin.y + size.y),
                  theme::col(half ? theme::Role::GfxOutline : isRegisterBoundary ? theme::Role::GfxGridMajor : theme::Role::GfxGrid), half ? 2.0f : 1.0f);
    }
    for (int rowIndex = 0; rowIndex <= (int)playfield.rows.size(); rowIndex++)
      drawList->AddLine(ImVec2(origin.x, origin.y + rowIndex * cellHeight), ImVec2(origin.x + size.x, origin.y + rowIndex * cellHeight), theme::col(theme::Role::GfxGrid));
    // the columns the kernel does not show, hatched; they cannot be drawn on
    auto hatch = [&](int fromColumn, int toColumn) {
      if (toColumn <= fromColumn) return;
      float x0 = origin.x + fromColumn * cellWidth, x1 = origin.x + toColumn * cellWidth;
      drawList->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + size.y), theme::col(theme::Role::GfxLocked));
      drawList->PushClipRect(ImVec2(x0, origin.y), ImVec2(x1, origin.y + size.y), true);
      for (float offset = -size.y; offset < x1 - x0; offset += 8) drawList->AddLine(ImVec2(x0 + offset, origin.y + size.y), ImVec2(x0 + offset + size.y, origin.y), theme::col(theme::Role::GfxLockedHatch));
      drawList->PopClipRect();
    };
    hatch(0, firstCol);
    hatch(lastCol + 1, 40);
    ImVec2 mouse = ImGui::GetMousePos();
    int mc = (int)std::floor((mouse.x - origin.x) / cellWidth), mr = (int)std::floor((mouse.y - origin.y) / cellHeight);
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
      paintValue_ = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 0 : !playfield.pixel(mr, mc);
    // a mirrored-only kernel: the right half is drawn through its left counterpart
    int drawColumn = mirrorOnly && mc >= 20 ? 39 - mc : mc;
    bool drawable = drawColumn >= firstCol && drawColumn <= lastCol;
    if (active && paintValue_ >= 0 && drawable && mr >= 0 && mr < (int)playfield.rows.size() && playfield.pixel(mr, drawColumn) != (paintValue_ == 1)) {
      if (mirrorOnly && (playfield.rows[mr].asymmetric || playfield.rows[mr].mode != PfMode::Mirror)) playfield.setAsymmetric(mr, false), playfield.rows[mr].mode = PfMode::Mirror;
      playfield.setPixel(mr, drawColumn, paintValue_ == 1);
      changed();
    }
    if (!active) paintValue_ = -1;
    if (hovered && !active && mc >= 0 && mc < 40) {
      int sourceColumn = mc < 20 ? mc : playfield.rows[std::clamp(mr, 0, (int)playfield.rows.size() - 1)].mode == PfMode::Mirror ? 39 - mc : mc - 20;
      int registerIndex, bitIndex;
      pfBit(sourceColumn, &registerIndex, &bitIndex);
      ImGui::SetTooltip("%s %d · %s %d · PF%d bit %d", T("row"), mr + 1, T("column"), mc + 1, registerIndex, bitIndex);
    }
    // per row: mode, asymmetric, colours
    float buttonHeight = std::min(cellHeight - 2, ImGui::GetFrameHeight());
    for (int rowIndex = 0; rowIndex < (int)playfield.rows.size(); rowIndex++) {
      PfRow& row = playfield.rows[rowIndex];
      ImGui::PushID(rowIndex);
      ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x + 8, origin.y + rowIndex * cellHeight + (cellHeight - buttonHeight) / 2));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, std::max(0.0f, (buttonHeight - ImGui::GetTextLineHeight()) / 2)));
      if (!mirrorOnly) {
      if (ImGui::Button(row.mode == PfMode::Mirror ? "M" : "R", ImVec2(buttonHeight + 6, buttonHeight))) {
        // keep what the screen shows: a symmetric row changes its right half, an asymmetric one keeps it
        bool wasAsymmetric = row.asymmetric;
        if (wasAsymmetric) {
          Playfield before = playfield;
          row.mode = row.mode == PfMode::Mirror ? PfMode::Repeat : PfMode::Mirror;
          uint64_t keep = row.bits & 0xFFFFFull;
          for (int column = 20; column < 40; column++)
            if (before.pixel(rowIndex, column)) keep |= 1ull << column;
          row.bits = keep;
        } else {
          row.mode = row.mode == PfMode::Mirror ? PfMode::Repeat : PfMode::Mirror;
        }
        changed();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.mode == PfMode::Mirror ? T("Mirror: the right half is the left one reversed") : T("Repeat: the right half is the left one again"));
      ImGui::SameLine(0, 2);
      bool asymmetric = row.asymmetric;
      ImGui::PushStyleColor(ImGuiCol_Button, asymmetric ? theme::colv(theme::Role::GfxAsymmetric) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
      if (ImGui::Button("A", ImVec2(buttonHeight + 6, buttonHeight))) playfield.setAsymmetric(rowIndex, !asymmetric), changed();
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", asymmetric ? T("Asymmetric: the right half is drawn on its own. Click to copy the left half again.") : T("Symmetric: the right half follows the left one"));
      ImGui::SameLine(0, 6);
      }
      ImGui::PopStyleVar();
      // the row colours the kernel can change
      int chosenColour;
      bool pfRow = pfRows, bkRow = bkRows;
      if (pfRow && colourButton("pf", rowIndex == 0 && row.pf < 0 ? playfield.pfColour : row.pf, tv, &chosenColour, rowIndex > 0, buttonHeight)) row.pf = chosenColour, changed();
      if (pfRow && bkRow) ImGui::SameLine(0, 2);
      if (bkRow && colourButton("bk", rowIndex == 0 && row.bk < 0 ? playfield.bkColour : row.bk, tv, &chosenColour, rowIndex > 0, buttonHeight)) row.bk = chosenColour, changed();
      ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x - 28, origin.y + size.y + 8));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  std::string help = mirrorOnly ? T("Left: draw or erase · Right: erase · the right half follows the left one")
                                : T("Left: draw or erase · Right: erase · M/R: mirror or repeat · A: asymmetric");
  if (pfRows && bkRows) help += std::string(" · ") + T("the two squares: playfield and background colours");
  else if (pfRows) help += std::string(" · ") + T("the square: the row's playfield colour");
  else if (bkRows) help += std::string(" · ") + T("the square: the row's background colour");
  ImGui::TextWrapped("%s", help.c_str());
  ImGui::PopStyleColor();
  };
  if (narrow) {
    // one column: the properties (folded), the canvas, the preview and the warnings
    ImGui::BeginChild("pfall", ImVec2(0, contentHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::CollapsingHeader(T("Playfield properties"))) props();
    canvas();
    preview(std::min(ImGui::GetContentRegionAvail().x, 480.0f));
    ImGui::EndChild();
    return;
  }
  ImGui::BeginChild("pfprops", ImVec2(em(250), contentHeight), ImGuiChildFlags_Borders);
  props();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("pfcanvas", ImVec2(std::max(160.0f, ImGui::GetContentRegionAvail().x - 278), contentHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  canvas();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("pfpreview", ImVec2(em(270), contentHeight), ImGuiChildFlags_Borders);
  preview(270);
  ImGui::EndChild();
}

}  // namespace vcs
