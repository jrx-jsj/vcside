// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "gfx_editor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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

static ImU32 col(Tv tv, int c, float alpha = 1.0f) {
  uint32_t v = rgb(tv, c);
  return IM_COL32((v >> 16) & 255, (v >> 8) & 255, v & 255, (int)(alpha * 255));
}

static ImU32 mix(ImU32 a, ImU32 b, float t) {
  auto ch = [&](int s) { return (int)(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t) << s; };
  return (ImU32)(ch(0) | ch(8) | ch(16) | (255u << 24));
}

static std::string readAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

Asset GfxEditor::blank() {
  Asset a;
  Sprite s;
  s.name = "sprite";
  Layer l;
  l.obj = Obj::P0;
  l.resize(1, 8);
  l.colours[0][0] = 0x1E;
  s.layers.push_back(l);
  a.sprites.push_back(s);
  return a;
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
  Kernel k;
  if (asset_.kernel.empty() || !kernelFromId(asset_.kernel, &k) || k == project.kernel) return project;
  Profile p;
  p.kernel = k;
  return p;
}

bool GfxEditor::save(const std::string& path, const Profile& project, std::string* error, std::string* generated) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (error) *error = F("Cannot write {0}", {path});
    return false;
  }
  out << toJson(asset_);
  out.close();
  path_ = path;
  if (Profile k = profile(project); !k.basic()) {
    // the tables for the program, next to the .vcsgfx
    fs::path p(path);
    std::string inc = (p.parent_path() / (p.stem().string() + (k.c ? ".gfx.h" : ".gfx.asm"))).string();
    std::ofstream g(inc, std::ios::binary);
    if (g) {
      g << (k.c ? cInclude(asset_, p.filename().string()) : dasmInclude(asset_, p.filename().string()));
      if (generated) *generated = inc;
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
  Cells c;
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    c.push_back({x0, y0});
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) err += dy, x0 += sx;
    if (e2 <= dx) err += dx, y0 += sy;
  }
  return c;
}

static Cells rectCells(int x0, int y0, int x1, int y1, bool filled) {
  Cells c;
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      if (filled || y == y0 || y == y1 || x == x0 || x == x1) c.push_back({x, y});
  return c;
}

// The cells whose centre falls inside the ellipse that fits the box; the outline keeps
// those with a neighbour outside, so it stays one pixel thick and closed.
static Cells ellipseCells(int x0, int y0, int x1, int y1, bool filled) {
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  float cx = (x0 + x1 + 1) * 0.5f, cy = (y0 + y1 + 1) * 0.5f, rx = (x1 - x0 + 1) * 0.5f, ry = (y1 - y0 + 1) * 0.5f;
  auto inside = [&](int x, int y) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    float u = (x + 0.5f - cx) / rx, v = (y + 0.5f - cy) / ry;
    return u * u + v * v <= 1.0f;
  };
  Cells c;
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      if (inside(x, y) && (filled || !inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) || !inside(x, y + 1)))
        c.push_back({x, y});
  return c;
}

// Every cell joined to (x, y) that has the same value, within w x h.
static Cells fillCells(int x, int y, int w, int h, const std::function<bool(int, int)>& get) {
  Cells c;
  if (x < 0 || y < 0 || x >= w || y >= h) return c;
  bool target = get(x, y);
  std::vector<char> seen((size_t)w * h, 0);
  std::vector<std::pair<int, int>> todo = {{x, y}};
  while (!todo.empty()) {
    auto [px, py] = todo.back();
    todo.pop_back();
    if (px < 0 || py < 0 || px >= w || py >= h || seen[(size_t)py * w + px] || get(px, py) != target) continue;
    seen[(size_t)py * w + px] = 1;
    c.push_back({px, py});
    todo.push_back({px + 1, py}), todo.push_back({px - 1, py}), todo.push_back({px, py + 1}), todo.push_back({px, py - 1});
  }
  return c;
}

// A tool's icon, drawn in a square button.
static void toolIcon(ImDrawList* dl, ImVec2 p, float s, int tool, ImU32 c) {
  float t = std::max(1.0f, s * 0.09f), m = s * 0.22f;
  ImVec2 a(p.x + m, p.y + m), b(p.x + s - m, p.y + s - m);
  switch (tool) {
    case 0: {  // pencil: a slanted body and its tip
      ImVec2 q[4] = {{b.x - s * 0.12f, a.y}, {b.x, a.y + s * 0.12f}, {a.x + s * 0.12f, b.y}, {a.x, b.y - s * 0.12f}};
      dl->AddConvexPolyFilled(q, 4, c);
      dl->AddTriangleFilled(ImVec2(a.x, b.y - s * 0.12f), ImVec2(a.x + s * 0.12f, b.y), ImVec2(a.x - s * 0.04f, b.y + s * 0.04f), c);
      break;
    }
    case 1: dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, a.y), c, t * 1.4f); break;
    case 2: dl->AddRect(a, b, c, 0, 0, t * 1.4f); break;
    case 3: dl->AddRectFilled(a, b, c); break;
    case 4: dl->AddEllipse(ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), ImVec2((b.x - a.x) / 2, (b.y - a.y) / 2.6f), c, 0, 0, t * 1.4f); break;
    case 5: dl->AddEllipseFilled(ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), ImVec2((b.x - a.x) / 2, (b.y - a.y) / 2.6f), c); break;
    case 6: {  // fill: a tipped bucket and a drop
      ImVec2 q[4] = {{a.x + s * 0.06f, a.y + s * 0.22f}, {a.x + s * 0.36f, a.y - s * 0.04f}, {b.x - s * 0.04f, a.y + s * 0.34f}, {a.x + s * 0.3f, b.y - s * 0.02f}};
      dl->AddConvexPolyFilled(q, 4, c);
      dl->AddCircleFilled(ImVec2(b.x - s * 0.02f, b.y - s * 0.06f), s * 0.09f, c);
      break;
    }
  }
}

bool GfxEditor::toolBar() {
  static const char* names[] = {N_("Pencil (P)"), N_("Line (L)"), N_("Rectangle (R)"), N_("Filled rectangle (Shift+R)"),
                                N_("Ellipse (E)"), N_("Filled ellipse (Shift+E)"), N_("Fill (F)")};
  bool picked = false;
  float s = ImGui::GetFrameHeight();
  for (int i = 0; i < 7; i++) {
    if (i) ImGui::SameLine(0, 2);
    ImGui::PushID(i);
    bool on = (int)tool_ == i;
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("tool", ImVec2(s, s))) tool_ = (Tool)i, picked = true;
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = theme::col(on ? theme::Role::UiButtonActive : hov ? theme::Role::UiButtonHover : theme::Role::UiButton);
    dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), bg, ImGui::GetStyle().FrameRounding);
    toolIcon(dl, p, s, i, theme::col(theme::Role::UiText));
    if (hov) ImGui::SetTooltip("%s", T(names[i]));
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
  uint32_t v = rgb(tv, colour < 0 ? 0 : colour);
  ImVec4 c((v >> 16 & 255) / 255.f, (v >> 8 & 255) / 255.f, (v & 255) / 255.f, colour < 0 ? 0.35f : 1.f);
  std::string tip = colour < 0 ? T("Same as the row above") : hexColour(colour);
  if (ImGui::ColorButton(tip.c_str(), c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreview, ImVec2(size, size)))
    ImGui::OpenPopup("palette");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip.c_str());
  if (ImGui::BeginPopup("palette")) {
    ImGui::TextDisabled("%s", T("Hue down, luminance across"));
    for (int h = 0; h < 16; h++) {
      for (int l = 0; l < 8; l++) {
        int value = h << 4 | l << 1;
        uint32_t p = rgb(tv, value);
        ImVec4 pc((p >> 16 & 255) / 255.f, (p >> 8 & 255) / 255.f, (p & 255) / 255.f, 1);
        if (l) ImGui::SameLine(0, 2);
        ImGui::PushID(value);
        bool selected = value == (colour & 0xFE);
        if (selected) ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2);
        if (ImGui::ColorButton(hexColour(value).c_str(), pc, ImGuiColorEditFlags_NoBorder * !selected, ImVec2(em(22), em(16)))) {
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
  k_ = profile(project);
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
  float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
  float right = ImGui::GetItemRectMax().x + (spacing < 0 ? ImGui::GetStyle().ItemSpacing.x : spacing) + w;
  if (right < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine(0, spacing);
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
    Sprite s = blank().sprites[0];
    s.name = "sprite" + std::to_string(asset_.sprites.size() + 1);
    asset_.sprites.push_back(s);
    item_ = (int)asset_.sprites.size() - 1, layer_ = frame_ = 0;
    changed();
  }
  nextTo(T("New playfield"));
  if (ImGui::Button(T("New playfield"))) {
    Playfield p;
    p.name = "playfield" + std::to_string(asset_.playfields.size() + 1);
    asset_.playfields.push_back(p);
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
  std::string kname = asset_.kernel.empty() ? F("Kernel: the project's ({0})", {project.describe()}) : F("Kernel: {0}", {kernelName(k_.kernel)});
  nextTo(kname.c_str(), 24);
  ImGui::SetNextItemWidth(std::min(ImGui::CalcTextSize(kname.c_str()).x + 40, 360.0f));
  if (ImGui::BeginCombo("##kernel", kname.c_str())) {
    if (ImGui::Selectable(F("The project's ({0})", {project.describe()}).c_str(), asset_.kernel.empty())) asset_.kernel.clear(), changed();
    for (int i = 0; i < 5; i++)
      if (ImGui::Selectable(kernelName((Kernel)i), asset_.kernel == kernelId((Kernel)i)))
        asset_.kernel = kernelId((Kernel)i), changed();
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", F("What draws these graphics: {0}. The editor offers what it can show, and the warnings and the batari Basic copy follow it. The options (kernel_options, pfres) are read from the project's main program.",
                              {k_.describe()}).c_str());
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
      Asset got;
      std::string what;
      if (clip && importText(clip, &got, &what, k_)) {
        int at = (int)asset_.sprites.size();
        asset_.sprites.insert(asset_.sprites.end(), got.sprites.begin(), got.sprites.end());
        asset_.playfields.insert(asset_.playfields.end(), got.playfields.begin(), got.playfields.end());
        item_ = got.sprites.empty() ? (int)(asset_.sprites.size() + asset_.playfields.size()) - 1 : at;
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
      copy(sprite ? exportBasic(asset_.sprites[item_], k_) : exportBasic(asset_.playfields[item_ - asset_.sprites.size()], k_), "batari Basic");
    }
    if (ImGui::MenuItem(T("batari Basic, kept up to date"))) {
      ImGui::SetClipboardText(basicRegion(fs::path(path_).filename().string(), asset_, item_, k_).c_str());
      message_ = T("Copied: paste it in the program; every save of this file rewrites it.");
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T("The blocks between `rem vcsgfx` lines. Saving this file rewrites them in the project's programs (one region per animation frame)."));
    ImGui::Separator();
    ImGui::TextDisabled("%s", k_.c ? T("On save, every item also goes to <file>.gfx.h for the program to include.")
                              : !k_.basic() ? T("On save, every item also goes to <file>.gfx.asm for the program to include.")
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
  for (auto& i : list) {
    ImVec4 c = theme::colv(i.severity == Issue::Error ? theme::Role::MsgError : i.severity == Issue::Warning ? theme::Role::MsgWarning : theme::Role::MsgInfo);
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::Bullet();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", i.text.c_str());
  }
}

// ---------------------------------------------------------------------------
// Sprites

static void spriteBounds(const Sprite& s, int b[4], int margin) {
  b[0] = 0, b[1] = 0, b[2] = 8, b[3] = 1;
  bool first = true;
  for (auto& l : s.layers) {
    int x0 = l.dx, y0 = l.dy, x1 = l.dx + l.width(), y1 = l.dy + l.height;
    if (first) b[0] = x0, b[1] = y0, b[2] = x1, b[3] = y1, first = false;
    b[0] = std::min(b[0], x0), b[1] = std::min(b[1], y0), b[2] = std::max(b[2], x1), b[3] = std::max(b[3], y1);
  }
  b[0] -= margin, b[1] -= margin, b[2] += margin, b[3] += margin;
}

// Draws the sprite as the TIA would: per pixel the first object in priority order
// (P0, M0, P1 and the players a kernel draws with it, M1, BL) that is on; a missile in the colour its player sets on that line.
void GfxEditor::drawSprite(const Sprite& s, int frame, int phase, ImVec2 o, float cw, float ch, Tv tv, int bk, int highlight,
                           int b[4]) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 bkc = col(tv, bk);
  dl->AddRectFilled(o, ImVec2(o.x + (b[2] - b[0]) * cw, o.y + (b[3] - b[1]) * ch), bkc);
  static const Obj order[] = {Obj::P0,  Obj::M0,  Obj::P1,  Obj::P2,  Obj::P3,  Obj::P4,  Obj::P5,
                              Obj::P6,  Obj::P7,  Obj::P8,  Obj::P9,  Obj::P10, Obj::P11, Obj::P12,
                              Obj::P13, Obj::P14, Obj::P15, Obj::P16, Obj::M1,  Obj::BL};
  for (int y = b[1]; y < b[3]; y++)
    for (int x = b[0]; x < b[2]; x++) {
      int winner = -1;
      for (Obj ob : order) {
        for (int i = 0; i < (int)s.layers.size() && winner < 0; i++) {
          const Layer& l = s.layers[i];
          if (l.obj != ob || (phase >= 0 && l.phase != phase)) continue;
          if (l.pixel(frame, y - l.dy, x - l.dx)) winner = i;
        }
        if (winner >= 0) break;
      }
      if (winner < 0) continue;
      const Layer& w = s.layers[winner];
      // as the kernel shows it: one colour for an object that cannot change it per row
      auto colourAt = [&](const Layer& l, int row) { return l.colour(frame, k_.rowColours(l.obj) ? row : 0); };
      int c = colourAt(w, y - w.dy);
      if (w.obj == Obj::M0 || w.obj == Obj::M1)
        for (auto& p : s.layers)
          if (p.obj == (w.obj == Obj::M0 ? Obj::P0 : Obj::P1) && p.phase == w.phase && y - p.dy >= 0 && y - p.dy < p.height)
            c = colourAt(p, y - p.dy);
      ImU32 pc = col(tv, c);
      if (highlight >= 0 && winner != highlight && othersDim_) pc = mix(pc, bkc, 0.55f);
      ImVec2 a(o.x + (x - b[0]) * cw, o.y + (y - b[1]) * ch);
      dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + ch), pc);
    }
}

// The label of an object in the menus: "P0  player", "M1  missile"...
static std::string objLabel(Obj o) {
  return std::string(objName(o)) + "  " + (isPlayer(o) ? T("player") : o == Obj::BL ? T("ball") : T("missile"));
}
// The objects in the order the menus show them: the TIA's five, then the extra players.
static std::vector<Obj> objectList(const Profile& k) {
  std::vector<Obj> v = {Obj::P0, Obj::P1, Obj::M0, Obj::M1, Obj::BL};
  for (int n = 2; n < k.players(); n++) v.push_back(playerObj(n));
  return v;
}

void GfxEditor::spriteView(Sprite& s, Tv tv) {
  layer_ = std::clamp(layer_, 0, std::max(0, (int)s.layers.size() - 1));
  frame_ = std::clamp(frame_, 0, s.frames - 1);
  float h = ImGui::GetContentRegionAvail().y;
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
    int pf = (tick / std::max(1, s.frameTicks)) % s.frames;
    if (anim_ >= 0 && anim_ < (int)s.animations.size() && !s.animations[anim_].steps.empty())
      pf = s.frameAt(s.animations[anim_], (int)((ImGui::GetTime() - animStart_) * 60));
    int phase = flicker_ && s.phases() > 1 ? tick % s.phases() : -1;
    int pb[4];
    spriteBounds(s, pb, 1);
    float pcw = (float)previewScale_, pch = previewScale_ * kLineAspect * s.lineHeight;
    ImVec2 po = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2((pb[2] - pb[0]) * pcw, (pb[3] - pb[1]) * pch));
    drawSprite(s, pf, phase, po, pcw, pch, tv, s.background, -1, pb);
    ImGui::SeparatorText(T("Warnings"));
    issues(check(s, k_));
  };

  // --- left: the sprite, its layers and the selected layer
  ImGui::BeginChild("layers", ImVec2(side, h), ImGuiChildFlags_Borders);
  std::string name = s.name;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue) || (ImGui::IsItemDeactivatedAfterEdit())) {
    if (!name.empty()) s.name = name, changed();
  }
  int lh = s.lineHeight;
  ImGui::SetNextItemWidth(em(110));
  if (ImGui::SliderInt(T("lines per row"), &lh, 1, 4)) s.lineHeight = lh, changed();
  int ticks = s.frameTicks;
  ImGui::SetNextItemWidth(em(110));
  if (ImGui::SliderInt(T("frames per step"), &ticks, 1, 30)) s.frameTicks = ticks, changed();
  ImGui::SeparatorText(T("Layers"));
  static const theme::Role objRole[] = {theme::Role::GfxP0, theme::Role::GfxP1, theme::Role::GfxM0, theme::Role::GfxM1, theme::Role::GfxBL};
  auto objCol = [&](Obj o) { return theme::colv((int)o < 5 ? objRole[(int)o] : theme::Role::GfxP1); };  // extra players: P1's family
  for (int i = 0; i < (int)s.layers.size(); i++) {
    auto& l = s.layers[i];
    ImGui::PushID(i);
    ImGui::PushStyleColor(ImGuiCol_Text, objCol(l.obj));
    ImGui::TextUnformatted(objName(l.obj));
    ImGui::PopStyleColor();
    ImGui::SameLine(36);
    std::string label = (l.name.empty() ? std::string(T("(no name)")) : l.name) + "##l";
    if (ImGui::Selectable(label.c_str(), i == layer_, 0, ImVec2(side - 110, 0))) layer_ = i;
    ImGui::SameLine(side - 62);
    ImGui::TextDisabled("%s", F("phase {0}", {std::to_string(l.phase + 1)}).c_str());
    ImGui::PopID();
  }
  if (ImGui::Button(T("Add"))) ImGui::OpenPopup("add");
  if (ImGui::BeginPopup("add")) {
    for (Obj o : objectList(k_)) {
      bool used = std::any_of(s.layers.begin(), s.layers.end(), [&](const Layer& l) { return l.obj == o && l.phase == 0; });
      std::string why = k_.missing(o);
      if (ImGui::MenuItem(objLabel(o).c_str(), used ? T("used in phase 1") : "", false, why.empty()) && why.empty()) {
        Layer l;
        l.obj = o;
        int hgt = s.layers.empty() ? 8 : s.layers[layer_].height;
        l.resize(s.frames, hgt);
        if (!s.layers.empty()) l.dx = s.layers[layer_].dx + s.layers[layer_].width() + 1, l.dy = s.layers[layer_].dy;
        // the first phase where the object is free
        for (int p = 0; p < 4; p++)
          if (std::none_of(s.layers.begin(), s.layers.end(), [&](const Layer& x) { return x.obj == o && x.phase == p; })) {
            l.phase = p;
            break;
          }
        l.colours.assign(s.frames, std::vector<int>(l.height, -1));
        for (auto& c : l.colours) c[0] = o == Obj::BL ? 0x0E : 0x46;
        s.layers.push_back(l);
        layer_ = (int)s.layers.size() - 1;
        changed();
      }
      if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(s.layers.size() <= 1);
  if (ImGui::Button(T("Remove"))) {
    s.layers.erase(s.layers.begin() + layer_);
    layer_ = std::max(0, layer_ - 1);
    changed();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::ArrowButton("up", ImGuiDir_Up) && layer_ > 0) std::swap(s.layers[layer_], s.layers[layer_ - 1]), layer_--, changed();
  ImGui::SameLine();
  if (ImGui::ArrowButton("down", ImGuiDir_Down) && layer_ + 1 < (int)s.layers.size())
    std::swap(s.layers[layer_], s.layers[layer_ + 1]), layer_++, changed();

  if (!s.layers.empty()) {
    Layer& l = s.layers[layer_];
    ImGui::SeparatorText(T("Layer"));
    Obj chosen = l.obj;
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::BeginCombo(T("object"), objName(l.obj))) {
      for (Obj o : objectList(k_)) {
        std::string why = k_.missing(o);
        if (ImGui::Selectable(objLabel(o).c_str(), o == l.obj, why.empty() ? 0 : ImGuiSelectableFlags_Disabled)) chosen = o;
        if (!why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
      }
      ImGui::EndCombo();
    }
    if (chosen != l.obj) {
      bool wasPlayer = isPlayer(l.obj);
      l.obj = chosen;
      if (wasPlayer != isPlayer(l.obj)) {
        l.size = 1;
        for (auto& f : l.bits)
          for (auto& r : f) r = r ? (isPlayer(l.obj) ? 0x80 : 1) : 0;
      }
      changed();
    }
    std::string ln = l.name;
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::InputText(T("name"), &ln)) l.name = ln, changed();
    int ph = l.phase;
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::Combo(T("phase"), &ph, "1\0" "2\0" "3\0" "4\0")) l.phase = ph, changed();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T("Layers in different phases are drawn on alternate frames (flicker), so one object can be used more than once."));
    int pos[2] = {l.dx, l.dy};
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::DragInt2(T("position"), pos, 0.2f, -64, 64)) l.dx = pos[0], l.dy = pos[1], changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Colour clocks and rows. Shift+drag on the canvas moves the layer."));
    if (isPlayer(l.obj)) {
      int sz = l.size == 4 ? 2 : l.size == 2 ? 1 : 0;
      ImGui::SetNextItemWidth(em(120));
      if (ImGui::Combo(T("width"), &sz, "1x\0" "2x\0" "4x\0")) l.size = 1 << sz, changed();
      bool rf = l.reflect;
      if (ImGui::Checkbox(T("reflected (REFP)"), &rf)) l.reflect = rf, changed();
    } else {
      int sz = l.size >= 8 ? 3 : l.size >= 4 ? 2 : l.size >= 2 ? 1 : 0;
      ImGui::SetNextItemWidth(em(120));
      if (ImGui::Combo(T("width"), &sz, "1\0" "2\0" "4\0" "8\0")) l.size = 1 << sz, changed();
    }
    int hg = l.height;
    ImGui::SetNextItemWidth(em(120));
    if (ImGui::DragInt(T("rows"), &hg, 0.2f, 1, 128)) l.resize(s.frames, hg), changed();
  }
  if (narrow) preview();
  ImGui::EndChild();
  ImGui::SameLine();

  // --- centre: the canvas, the row colours of the layer and the frames
  float rightW = narrow ? 0 : em(270);
  float centreW = std::max(160.0f, ImGui::GetContentRegionAvail().x - rightW - (narrow ? 0 : 8));
  ImGui::BeginChild("canvas", ImVec2(centreW, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  toolBar();
  ImGui::SetNextItemWidth(em(120));
  ImGui::SliderFloat(T("zoom"), &zoom_, 6, 32, "%.0f");
  ImGui::SameLine();
  int nb;
  if (colourButton("bk", s.background, tv, &nb, false, ImGui::GetFrameHeight())) s.background = nb, changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Only to see the sprite against it: the program sets its own COLUBK."));
  ImGui::SameLine();
  ImGui::TextUnformatted(T("background"));
  ImGui::SameLine();
  ImGui::Checkbox(T("dim the other layers"), &othersDim_);
  int b[4];
  spriteBounds(s, b, 2);
  float cw = zoom_, ch = zoom_ * kLineAspect * s.lineHeight;
  ImVec2 o = ImGui::GetCursorScreenPos();
  ImVec2 size((b[2] - b[0]) * cw, (b[3] - b[1]) * ch);
  ImGui::InvisibleButton("grid", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  drawSprite(s, frame_, -1, o, cw, ch, tv, s.background, layer_, b);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int x = b[0]; x <= b[2]; x++)
    dl->AddLine(ImVec2(o.x + (x - b[0]) * cw, o.y), ImVec2(o.x + (x - b[0]) * cw, o.y + size.y), theme::col(x % 8 == 0 ? theme::Role::GfxGridMajor : theme::Role::GfxGrid));
  for (int y = b[1]; y <= b[3]; y++)
    dl->AddLine(ImVec2(o.x, o.y + (y - b[1]) * ch), ImVec2(o.x + size.x, o.y + (y - b[1]) * ch), theme::col(theme::Role::GfxGrid));
  if (!s.layers.empty()) {
    Layer& l = s.layers[layer_];
    ImVec2 a(o.x + (l.dx - b[0]) * cw, o.y + (l.dy - b[1]) * ch);
    dl->AddRect(a, ImVec2(a.x + l.width() * cw, a.y + l.height * ch), theme::col(theme::Role::GfxOutline), 0, 0, 1.5f);
    // painting: the pencil sets or clears what was under the first click, the other
    // tools draw with the left button and erase with the right; Shift+drag moves
    ImVec2 m = ImGui::GetMousePos();
    int mx = b[0] + (int)std::floor((m.x - o.x) / cw), my = b[1] + (int)std::floor((m.y - o.y) / ch);
    int lx = mx - l.dx, ly = my - l.dy;
    auto setPx = [&](int x, int y, bool on) {
      if (x < 0 || x >= l.width() || y < 0 || y >= l.height) return;
      uint8_t& v = l.bits[frame_][y];
      uint8_t nv = v;
      if (isPlayer(l.obj)) {
        int bit = x / l.size;
        if (l.reflect) bit = 7 - bit;
        nv = on ? v | (0x80 >> bit) : v & ~(0x80 >> bit);
      } else {
        nv = on ? 1 : 0;
      }
      if (nv != v) v = nv, changed();
    };
    bool pressed = hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right));
    bool right = ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (pressed) {
      if (ImGui::GetIO().KeyShift && tool_ == Tool::Pen) {
        moving_ = true;
        moveDx_ = mx - l.dx, moveDy_ = my - l.dy;
      } else if (tool_ == Tool::Pen) {
        paintValue_ = right ? 0 : !l.pixel(frame_, ly, lx);
      } else if (tool_ == Tool::Fill) {
        bool on = !right && !l.pixel(frame_, ly, lx);
        for (auto [x, y] : fillCells(lx, ly, l.width(), l.height, [&](int x, int y) { return l.pixel(frame_, y, x); }))
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
      if (l.dx != mx - moveDx_ || l.dy != my - moveDy_) l.dx = mx - moveDx_, l.dy = my - moveDy_, changed();
    } else if (active && paintValue_ >= 0 && tool_ == Tool::Pen) {
      setPx(lx, ly, paintValue_);
    } else if (active && paintValue_ >= 0 && shapeTool) {
      // the shape as it would be, over the canvas, until the button is let go
      ImU32 pc = theme::col(paintValue_ ? theme::Role::GfxOutline : theme::Role::GfxLockedHatch);
      for (auto [x, y] : shape())
        if (x >= 0 && x < l.width() && y >= 0 && y < l.height) {
          ImVec2 q(o.x + (l.dx + x - b[0]) * cw, o.y + (l.dy + y - b[1]) * ch);
          dl->AddRectFilled(ImVec2(q.x + 1, q.y + 1), ImVec2(q.x + cw - 1, q.y + ch - 1), pc);
        }
    }
    if (paintValue_ >= 0 && shapeTool && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      for (auto [x, y] : shape()) setPx(x, y, paintValue_);
      paintValue_ = -1;
    }
    if (!active && !(paintValue_ >= 0 && shapeTool)) paintValue_ = -1, moving_ = false;
    if (hovered && !active)
      ImGui::SetTooltip("x %d, %s %d", mx - l.dx, T("row"), my - l.dy + 1);

    // the row colours of the layer, next to its rows
    ImVec2 end = ImGui::GetCursorScreenPos();
    float sw = std::min(ch - 2, 18.0f);
    int colourRows = k_.rowColours(l.obj) ? l.height : 1;  // otherwise one colour for the object
    for (int r = 0; r < colourRows; r++) {
      ImGui::SetCursorScreenPos(ImVec2(o.x + size.x + 8, o.y + (l.dy - b[1] + r) * ch + (ch - sw) / 2));
      int c = l.colours[frame_][r], nc;
      std::string id = "c" + std::to_string(r);
      if (colourButton(id.c_str(), r == 0 && c < 0 ? l.colour(frame_, 0) : c, tv, &nc, r > 0, sw)) {
        if (ImGui::GetIO().KeyAlt) {
          for (int f = 0; f < s.frames; f++) l.colours[f][r] = nc;  // Alt: every frame
        } else {
          l.colours[frame_][r] = nc;
        }
        changed();
      }
    }
    if (colourRows == 1 && l.height > 1 && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", F("{0}: one colour for the whole object in this kernel.", {objName(l.obj)}).c_str());
    ImGui::SetCursorScreenPos(end);
  }
  ImGui::Dummy(ImVec2(0, 6));
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", T("Left: draw · Right: erase · Shift+drag with the pencil: move the layer · Alt+colour: every frame"));
  ImGui::PopStyleColor();

  // frames
  ImGui::SeparatorText(T("Frames"));
  int tb[4];
  spriteBounds(s, tb, 0);
  float tcw = 3, tch = 3 * kLineAspect * s.lineHeight;
  for (int f = 0; f < s.frames; f++) {
    ImGui::PushID(f);
    if (f) ImGui::SameLine();
    ImVec2 to = ImGui::GetCursorScreenPos();
    ImVec2 ts((tb[2] - tb[0]) * tcw + 6, (tb[3] - tb[1]) * tch + 6);
    if (ImGui::InvisibleButton("f", ts)) frame_ = f;
    drawSprite(s, f, -1, ImVec2(to.x + 3, to.y + 3), tcw, tch, tv, s.background, -1, tb);
    dl->AddRect(to, ImVec2(to.x + ts.x, to.y + ts.y), theme::col(f == frame_ ? theme::Role::GfxThumbSelected : theme::Role::GfxThumbFrame), 3, 0,
                f == frame_ ? 2.0f : 1.0f);
    ImGui::PopID();
  }
  if (ImGui::Button(T("New frame"))) s.insertFrame(frame_ + 1, -1), frame_++, changed();
  ImGui::SameLine();
  if (ImGui::Button(T("Duplicate"))) s.insertFrame(frame_ + 1, frame_), frame_++, changed();
  ImGui::SameLine();
  ImGui::BeginDisabled(s.frames <= 1);
  if (ImGui::Button(T("Delete frame"))) s.removeFrame(frame_), frame_ = std::min(frame_, s.frames - 1), changed();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::ArrowButton("fl", ImGuiDir_Left) && frame_ > 0) s.swapFrames(frame_, frame_ - 1), frame_--, changed();
  ImGui::SameLine();
  if (ImGui::ArrowButton("fr", ImGuiDir_Right) && frame_ + 1 < s.frames) s.swapFrames(frame_, frame_ + 1), frame_++, changed();
  animations(s, tv, tb, tcw, tch);
  ImGui::EndChild();
  if (!narrow) {
    ImGui::SameLine();
    ImGui::BeginChild("preview", ImVec2(rightW, h), ImGuiChildFlags_Borders);
    preview();
    ImGui::EndChild();
  }
}

// Animations: named sequences of steps, each a frame shown for some video frames.
void GfxEditor::animations(Sprite& s, Tv tv, int tb[4], float tcw, float tch) {
  ImGui::SeparatorText(T("Animations"));
  anim_ = std::clamp(anim_, 0, std::max(0, (int)s.animations.size() - 1));
  if (!s.animations.empty()) {
    ImGui::SetNextItemWidth(em(150));
    if (ImGui::BeginCombo("##anim", s.animations[anim_].name.c_str())) {
      for (int i = 0; i < (int)s.animations.size(); i++)
        if (ImGui::Selectable((s.animations[i].name + "##" + std::to_string(i)).c_str(), i == anim_)) anim_ = i, step_ = 0, animStart_ = ImGui::GetTime();
      ImGui::EndCombo();
    }
    ImGui::SameLine();
  }
  if (ImGui::Button(T("New animation"))) {
    Animation an;
    an.name = s.animations.empty() ? "walk" : "anim" + std::to_string(s.animations.size() + 1);
    for (int f = 0; f < s.frames; f++) an.steps.push_back({f, s.frameTicks});
    s.animations.push_back(an);
    anim_ = (int)s.animations.size() - 1, step_ = 0, animStart_ = ImGui::GetTime();
    changed();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Starts with every frame in order; add, remove and reorder steps below."));
  if (s.animations.empty()) {
    ImGui::TextDisabled("%s", T("No animations: the preview shows the frames in order."));
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button(T("Delete animation"))) {
    s.animations.erase(s.animations.begin() + anim_);
    anim_ = std::max(0, anim_ - 1);
    changed();
    return;
  }
  Animation& an = s.animations[anim_];
  std::string name = an.name;
  ImGui::SetNextItemWidth(em(150));
  if (ImGui::InputText((std::string(T("name")) + "##anim").c_str(), &name) && !name.empty()) an.name = name, changed();
  ImGui::SameLine();
  if (ImGui::Checkbox(T("loops"), &an.loop)) changed(), animStart_ = ImGui::GetTime();
  char secs[16];
  std::snprintf(secs, sizeof secs, "%.2f", an.length() / 60.0);
  std::string length = F("{0} video frames ({1} s on NTSC)", {std::to_string(an.length()), secs});
  nextTo(length.c_str());
  ImGui::TextDisabled("%s", length.c_str());
  // the steps: the frame each shows and for how long
  step_ = std::clamp(step_, 0, std::max(0, (int)an.steps.size() - 1));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float thumbW = std::max(30.0f, (tb[2] - tb[0]) * tcw + 6);
  for (int i = 0; i < (int)an.steps.size(); i++) {
    AnimStep& st = an.steps[i];
    ImGui::PushID(i + 1000);
    if (i && ImGui::GetItemRectMax().x + thumbW + 8 < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
    ImGui::BeginGroup();
    ImVec2 to = ImGui::GetCursorScreenPos();
    ImVec2 ts(thumbW, (tb[3] - tb[1]) * tch + 6);
    if (ImGui::InvisibleButton("s", ts)) step_ = i, frame_ = std::clamp(st.frame, 0, s.frames - 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", F("Step {0}: frame {1}", {std::to_string(i + 1), std::to_string(st.frame + 1)}).c_str());
    drawSprite(s, std::clamp(st.frame, 0, s.frames - 1), -1, ImVec2(to.x + 3, to.y + 3), tcw, tch, tv, s.background, -1, tb);
    dl->AddRect(to, ImVec2(to.x + ts.x, to.y + ts.y), theme::col(i == step_ ? theme::Role::GfxStepSelected : theme::Role::GfxThumbFrame), 3, 0,
                i == step_ ? 2.0f : 1.0f);
    int t = st.ticks;
    ImGui::SetNextItemWidth(thumbW);
    if (ImGui::DragInt("##t", &t, 0.2f, 1, 255)) st.ticks = t, changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Video frames this step lasts (60 a second on NTSC, 50 on PAL)"));
    ImGui::EndGroup();
    ImGui::PopID();
  }
  if (ImGui::Button(F("Add frame {0}", {std::to_string(frame_ + 1)}).c_str())) {
    int ticks = an.steps.empty() ? s.frameTicks : an.steps[step_].ticks;
    an.steps.insert(an.steps.begin() + (an.steps.empty() ? 0 : step_ + 1), AnimStep{frame_, ticks});
    step_ = an.steps.empty() ? 0 : step_ + 1;
    changed();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("A step with the frame being edited, after the selected step"));
  ImGui::SameLine();
  ImGui::BeginDisabled(an.steps.empty());
  if (ImGui::Button(T("Remove step"))) an.steps.erase(an.steps.begin() + step_), step_ = std::max(0, step_ - 1), changed();
  ImGui::SameLine();
  if (ImGui::ArrowButton("sl", ImGuiDir_Left) && step_ > 0) std::swap(an.steps[step_], an.steps[step_ - 1]), step_--, changed();
  ImGui::SameLine();
  if (ImGui::ArrowButton("sr", ImGuiDir_Right) && step_ + 1 < (int)an.steps.size()) std::swap(an.steps[step_], an.steps[step_ + 1]), step_++, changed();
  ImGui::EndDisabled();
  if (k_.basic()) {
    ImGui::TextUnformatted(T("batari Basic variables:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em(60));
    if (ImGui::InputText(T("counter"), &an.basicTimer)) changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Two variables the program does not use for anything else (a-z or ones it has dimmed)."));
    nextTo((std::string("12345678") + T("step")).c_str());
    ImGui::SetNextItemWidth(em(60));
    if (ImGui::InputText(T("step"), &an.basicStep)) changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Two variables the program does not use for anything else (a-z or ones it has dimmed)."));
  }
}

// ---------------------------------------------------------------------------
// Playfields

void GfxEditor::playfieldView(Playfield& p, Tv tv) {
  // the columns this kernel shows (a multisprite row: the left half, mirrored)
  int firstCol = k_.pfFirst(), lastCol = 39 - firstCol;
  bool mirrorOnly = k_.pfMirrorOnly();
  // what the kernel shows: its columns (mirrored if it can only mirror) and the colours it can change
  auto shown = [&](int r, int c) {
    if (mirrorOnly && c >= 20) c = 39 - c;
    return c >= firstCol && c <= lastCol && p.pixel(r, c);
  };
  bool pfRows = k_.pfRowColours(), bkRows = k_.bkRowColours();
  auto pfShown = [&](int r) { return p.pfAt(pfRows ? r : 0); };
  auto bkShown = [&](int r) { return p.bkAt(bkRows ? r : 0); };
  float h = ImGui::GetContentRegionAvail().y;
  bool narrow = ImGui::GetContentRegionAvail().x < 1000;
  auto preview = [&](float width) {
    ImGui::SeparatorText(T("Preview"));
    ImVec2 po = ImGui::GetCursorScreenPos();
    float pcw = (width - 20) / 40, pch = std::max(1.0f, pcw / 4 * kLineAspect * p.lineHeight);
    ImGui::Dummy(ImVec2(40 * pcw, p.rows.size() * pch));
    ImDrawList* pdl = ImGui::GetWindowDrawList();
    for (int r = 0; r < (int)p.rows.size(); r++) {
      float y = po.y + r * pch;
      pdl->AddRectFilled(ImVec2(po.x, y), ImVec2(po.x + 40 * pcw, y + pch), col(tv, bkShown(r)));
      for (int c = 0; c < 40; c++)
        if (shown(r, c)) pdl->AddRectFilled(ImVec2(po.x + c * pcw, y), ImVec2(po.x + (c + 1) * pcw, y + pch), col(tv, pfShown(r)));
    }
    ImGui::SeparatorText(T("Warnings"));
    issues(check(p, k_));
  };
  auto props = [&] {
    std::string name = p.name;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##name", &name) && !name.empty()) p.name = name, changed();
    int rows = (int)p.rows.size();
    ImGui::SetNextItemWidth(em(110));
    if (ImGui::DragInt(T("rows"), &rows, 0.2f, 1, k_.pfRowsMax())) {
      PfRow last = p.rows.back();
      last.pf = last.bk = -1;
      p.rows.resize(rows, last);
      changed();
    }
    if (k_.pfRowsDefault()) {
      ImGui::SameLine();
      if (ImGui::SmallButton(std::to_string(k_.pfRowsDefault()).c_str())) {
        PfRow last = p.rows.back();
        last.pf = last.bk = -1;
        p.rows.resize(k_.pfRowsDefault(), last);
        changed();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The rows the program shows"));
    }
    int lh = p.lineHeight;
    ImGui::SetNextItemWidth(em(110));
    if (ImGui::DragInt(T("lines per row"), &lh, 0.2f, 1, 64)) p.lineHeight = lh, changed();
    int nc;
    if (colourButton("pf", p.pfColour, tv, &nc, false, ImGui::GetFrameHeight())) p.pfColour = nc, changed();
    ImGui::SameLine();
    ImGui::TextUnformatted(T("playfield (COLUPF)"));
    if (colourButton("bk", p.bkColour, tv, &nc, false, ImGui::GetFrameHeight())) p.bkColour = nc, changed();
    ImGui::SameLine();
    ImGui::TextUnformatted(T("background (COLUBK)"));
    if (mirrorOnly) {
      ImGui::Dummy(ImVec2(0, 8));
      if (ImGui::Button(T("Mirror every row"))) {
        for (int r = 0; r < (int)p.rows.size(); r++) p.setAsymmetric(r, false), p.rows[r].mode = PfMode::Mirror;
        changed();
      }
      ImGui::Dummy(ImVec2(0, 8));
      ImGui::TextWrapped("%s", T("This kernel draws the left half from column 5 and mirrors it: draw on the left, the right half follows."));
      return;
    }
    ImGui::SeparatorText(T("All rows"));
    if (ImGui::Button(T("Mirror"))) {
      for (int r = 0; r < (int)p.rows.size(); r++)
        if (!p.rows[r].asymmetric) p.rows[r].mode = PfMode::Mirror;
      changed();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Repeat"))) {
      for (int r = 0; r < (int)p.rows.size(); r++)
        if (!p.rows[r].asymmetric) p.rows[r].mode = PfMode::Repeat;
      changed();
    }
    if (ImGui::Button(T("Symmetric (drop the right halves)"))) {
      for (int r = 0; r < (int)p.rows.size(); r++) p.setAsymmetric(r, false);
      changed();
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextWrapped("%s", T("Each row has its own mode (M mirror, R repeat), an A when its right half is drawn on its own, and its colours. Drawing on the right half of a symmetric row makes it asymmetric."));
  };
  auto canvas = [&] {
    ImGui::SetNextItemWidth(em(120));
    ImGui::SliderFloat(T("zoom"), &zoom_, 6, 32, "%.0f");
    ImGui::SameLine();
    int nbk;
    if (colourButton("bkc", p.bkColour, tv, &nbk, false, ImGui::GetFrameHeight())) p.bkColour = nbk, changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("COLUBK of the rows that do not set their own"));
    ImGui::SameLine();
    ImGui::TextUnformatted(T("background (COLUBK)"));
    // the 40 columns fit the space left by the row numbers and the row buttons; the zoom is the most
    float rowButtons = 4 * (ImGui::GetFrameHeight() + 8) + 16;
    float cw = std::max(9.0f, std::min(zoom_ * 1.2f, (ImGui::GetContentRegionAvail().x - 28 - rowButtons) / 40));
    // rows keep the TV's proportion unless that leaves no room for their buttons (the preview keeps it always)
    float ch = std::max(ImGui::GetFrameHeight() + 2, cw / 4 * kLineAspect * p.lineHeight);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // register names over the columns
    ImVec2 top = ImGui::GetCursorScreenPos();
    bool shortNames = 4 * cw < ImGui::CalcTextSize("PF0").x + 4;
    const char* left[] = {shortNames ? "0" : "PF0", shortNames ? "1" : "PF1", shortNames ? "2" : "PF2"};
    int starts[] = {0, 4, 12, 20};
    for (int k = 0; k < 3; k++) {
      dl->AddText(ImVec2(top.x + 28 + starts[k] * cw + 2, top.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), left[k]);
      dl->AddText(ImVec2(top.x + 28 + (20 + starts[k]) * cw + 2, top.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), left[k]);
    }
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight() + 2));
    ImVec2 o = ImGui::GetCursorScreenPos();
    o.x += 28;  // row numbers
    ImVec2 size(40 * cw, p.rows.size() * ch);
    ImGui::SetCursorScreenPos(o);
    ImGui::InvisibleButton("grid", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    for (int r = 0; r < (int)p.rows.size(); r++) {
      float y = o.y + r * ch;
      dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + size.x, y + ch), col(tv, bkShown(r)));
      ImU32 fg = col(tv, pfShown(r));
      for (int c = 0; c < 40; c++)
        if (p.pixel(r, c)) {
          ImU32 pc = c >= 20 && !p.rows[r].asymmetric ? mix(fg, col(tv, bkShown(r)), 0.25f) : fg;  // a copy of the left half
          dl->AddRectFilled(ImVec2(o.x + c * cw, y), ImVec2(o.x + (c + 1) * cw, y + ch), pc);
        }
      char num[8];
      std::snprintf(num, sizeof num, "%d", r + 1);
      dl->AddText(ImVec2(o.x - 26, y + (ch - ImGui::GetTextLineHeight()) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), num);
    }
    for (int c = 0; c <= 40; c++) {
      bool reg = c == 4 || c == 12 || c == 24 || c == 32, half = c == 20;
      dl->AddLine(ImVec2(o.x + c * cw, o.y), ImVec2(o.x + c * cw, o.y + size.y),
                  theme::col(half ? theme::Role::GfxOutline : reg ? theme::Role::GfxGridMajor : theme::Role::GfxGrid), half ? 2.0f : 1.0f);
    }
    for (int r = 0; r <= (int)p.rows.size(); r++)
      dl->AddLine(ImVec2(o.x, o.y + r * ch), ImVec2(o.x + size.x, o.y + r * ch), theme::col(theme::Role::GfxGrid));
    // the columns the kernel does not show, hatched; they cannot be drawn on
    auto hatch = [&](int c0, int c1) {
      if (c1 <= c0) return;
      float x0 = o.x + c0 * cw, x1 = o.x + c1 * cw;
      dl->AddRectFilled(ImVec2(x0, o.y), ImVec2(x1, o.y + size.y), theme::col(theme::Role::GfxLocked));
      dl->PushClipRect(ImVec2(x0, o.y), ImVec2(x1, o.y + size.y), true);
      for (float d = -size.y; d < x1 - x0; d += 8) dl->AddLine(ImVec2(x0 + d, o.y + size.y), ImVec2(x0 + d + size.y, o.y), theme::col(theme::Role::GfxLockedHatch));
      dl->PopClipRect();
    };
    hatch(0, firstCol);
    hatch(lastCol + 1, 40);
    ImVec2 m = ImGui::GetMousePos();
    int mc = (int)std::floor((m.x - o.x) / cw), mr = (int)std::floor((m.y - o.y) / ch);
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
      paintValue_ = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 0 : !p.pixel(mr, mc);
    // a mirrored-only kernel: the right half is drawn through its left counterpart
    int pc = mirrorOnly && mc >= 20 ? 39 - mc : mc;
    bool drawable = pc >= firstCol && pc <= lastCol;
    if (active && paintValue_ >= 0 && drawable && mr >= 0 && mr < (int)p.rows.size() && p.pixel(mr, pc) != (paintValue_ == 1)) {
      if (mirrorOnly && (p.rows[mr].asymmetric || p.rows[mr].mode != PfMode::Mirror)) p.setAsymmetric(mr, false), p.rows[mr].mode = PfMode::Mirror;
      p.setPixel(mr, pc, paintValue_ == 1);
      changed();
    }
    if (!active) paintValue_ = -1;
    if (hovered && !active && mc >= 0 && mc < 40) {
      int j = mc < 20 ? mc : p.rows[std::clamp(mr, 0, (int)p.rows.size() - 1)].mode == PfMode::Mirror ? 39 - mc : mc - 20;
      int reg, bit;
      pfBit(j, &reg, &bit);
      ImGui::SetTooltip("%s %d · %s %d · PF%d bit %d", T("row"), mr + 1, T("column"), mc + 1, reg, bit);
    }
    // per row: mode, asymmetric, colours
    float bh = std::min(ch - 2, ImGui::GetFrameHeight());
    for (int r = 0; r < (int)p.rows.size(); r++) {
      PfRow& row = p.rows[r];
      ImGui::PushID(r);
      ImGui::SetCursorScreenPos(ImVec2(o.x + size.x + 8, o.y + r * ch + (ch - bh) / 2));
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, std::max(0.0f, (bh - ImGui::GetTextLineHeight()) / 2)));
      if (!mirrorOnly) {
      if (ImGui::Button(row.mode == PfMode::Mirror ? "M" : "R", ImVec2(bh + 6, bh))) {
        // keep what the screen shows: a symmetric row changes its right half, an asymmetric one keeps it
        bool asym = row.asymmetric;
        if (asym) {
          Playfield before = p;
          row.mode = row.mode == PfMode::Mirror ? PfMode::Repeat : PfMode::Mirror;
          uint64_t keep = row.bits & 0xFFFFFull;
          for (int c = 20; c < 40; c++)
            if (before.pixel(r, c)) keep |= 1ull << c;
          row.bits = keep;
        } else {
          row.mode = row.mode == PfMode::Mirror ? PfMode::Repeat : PfMode::Mirror;
        }
        changed();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.mode == PfMode::Mirror ? T("Mirror: the right half is the left one reversed") : T("Repeat: the right half is the left one again"));
      ImGui::SameLine(0, 2);
      bool asym = row.asymmetric;
      ImGui::PushStyleColor(ImGuiCol_Button, asym ? theme::colv(theme::Role::GfxAsymmetric) : ImGui::GetStyle().Colors[ImGuiCol_Button]);
      if (ImGui::Button("A", ImVec2(bh + 6, bh))) p.setAsymmetric(r, !asym), changed();
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", asym ? T("Asymmetric: the right half is drawn on its own. Click to copy the left half again.") : T("Symmetric: the right half follows the left one"));
      ImGui::SameLine(0, 6);
      }
      ImGui::PopStyleVar();
      // the row colours the kernel can change
      int c;
      bool pfRow = pfRows, bkRow = bkRows;
      if (pfRow && colourButton("pf", r == 0 && row.pf < 0 ? p.pfColour : row.pf, tv, &c, r > 0, bh)) row.pf = c, changed();
      if (pfRow && bkRow) ImGui::SameLine(0, 2);
      if (bkRow && colourButton("bk", r == 0 && row.bk < 0 ? p.bkColour : row.bk, tv, &c, r > 0, bh)) row.bk = c, changed();
      ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x - 28, o.y + size.y + 8));
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
    ImGui::BeginChild("pfall", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::CollapsingHeader(T("Playfield properties"))) props();
    canvas();
    preview(std::min(ImGui::GetContentRegionAvail().x, 480.0f));
    ImGui::EndChild();
    return;
  }
  ImGui::BeginChild("pfprops", ImVec2(em(250), h), ImGuiChildFlags_Borders);
  props();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("pfcanvas", ImVec2(std::max(160.0f, ImGui::GetContentRegionAvail().x - 278), h), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  canvas();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("pfpreview", ImVec2(em(270), h), ImGuiChildFlags_Borders);
  preview(270);
  ImGui::EndChild();
}

}  // namespace vcs
