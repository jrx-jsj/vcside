// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "file_icons.h"

#include <algorithm>
#include <cctype>

#include "imgui.h"
#include "theme_ui.h"

namespace vcs::files {

static std::string lowerCase(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

Kind kindOf(const std::string& filename, bool directory) {
  if (directory) return Kind::Folder;
  std::string n = lowerCase(filename);
  size_t dot = n.rfind('.');
  std::string ext = dot == std::string::npos ? std::string() : n.substr(dot);
  if (ext == ".vcsproj") return Kind::Project;
  if (ext == ".asm" || ext == ".s" || ext == ".a65" || ext == ".dasm") return Kind::Asm;
  if (ext == ".c") return Kind::C;
  if (ext == ".h" || ext == ".inc" || ext == ".equ") return Kind::Header;
  if (ext == ".bas" || ext == ".bb") return Kind::Basic;
  if (ext == ".vcsgfx") return Kind::Graphics;
  if (ext == ".bin" || ext == ".a26" || ext == ".rom" || ext == ".elf") return Kind::Rom;
  if (ext == ".png" || ext == ".bmp" || ext == ".gif" || ext == ".jpg" || ext == ".jpeg") return Kind::Image;
  if (ext == ".lst" || ext == ".sym" || ext == ".map" || ext == ".lbl") return Kind::Listing;
  return Kind::Text;
}

static ImU32 withAlpha(ImU32 c, float a) {
  return (c & ~IM_COL32_A_MASK) | ((ImU32)(((c >> IM_COL32_A_SHIFT) & 255) * a) << IM_COL32_A_SHIFT);
}

// A sheet with its top right corner folded; a letter on it says the language.
static void page(ImDrawList* dl, ImVec2 p, float s, ImU32 c, const char* letter) {
  float x0 = p.x + s * 0.17f, x1 = p.x + s * 0.83f, y0 = p.y + s * 0.05f, y1 = p.y + s * 0.95f, f = s * 0.26f;
  ImVec2 pts[5] = {{x0, y0}, {x1 - f, y0}, {x1, y0 + f}, {x1, y1}, {x0, y1}};
  if (letter) {
    dl->AddConvexPolyFilled(pts, 5, c);
    dl->AddTriangleFilled(ImVec2(x1 - f, y0), ImVec2(x1 - f, y0 + f), ImVec2(x1, y0 + f), IM_COL32(0, 0, 0, 90));
    ImFont* font = ImGui::GetFont();
    float fs = s * 0.62f;
    ImVec2 ts = font->CalcTextSizeA(fs, 100, 0, letter);
    float lum = (0.299f * (c & 255) + 0.587f * ((c >> 8) & 255) + 0.114f * ((c >> 16) & 255)) / 255;  // ImU32 is ABGR
    ImU32 ink = lum > 0.55f ? IM_COL32(0, 0, 0, 210) : IM_COL32(255, 255, 255, 235);
    dl->AddText(font, fs, ImVec2((x0 + x1 - ts.x) * 0.5f, y1 - ts.y - s * 0.04f), ink, letter);
  } else {  // plain text: an outline with lines
    float t = std::max(1.0f, s * 0.07f);
    dl->AddPolyline(pts, 5, c, ImDrawFlags_Closed, t);
    dl->AddLine(ImVec2(x1 - f, y0), ImVec2(x1 - f, y0 + f), c, t);
    dl->AddLine(ImVec2(x1 - f, y0 + f), ImVec2(x1, y0 + f), c, t);
    for (int i = 0; i < 3; i++) {
      float y = y0 + s * (0.42f + i * 0.15f);
      dl->AddLine(ImVec2(x0 + s * 0.12f, y), ImVec2(x1 - s * (i == 2 ? 0.25f : 0.12f), y), withAlpha(c, 0.8f), t);
    }
  }
}

static void folder(ImDrawList* dl, ImVec2 p, float s, ImU32 c, bool open) {
  float x0 = p.x + s * 0.06f, x1 = p.x + s * 0.94f, y0 = p.y + s * 0.18f, y1 = p.y + s * 0.84f, r = s * 0.08f;
  // the tab
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + s * 0.38f, y0 + s * 0.2f), c, r, ImDrawFlags_RoundCornersTop);
  if (!open) {
    dl->AddRectFilled(ImVec2(x0, y0 + s * 0.12f), ImVec2(x1, y1), c, r);
    return;
  }
  // open: the back, then the front leaning towards the viewer
  dl->AddRectFilled(ImVec2(x0, y0 + s * 0.12f), ImVec2(x1 - s * 0.06f, y1), withAlpha(c, 0.55f), r);
  ImVec2 front[4] = {{x0 + s * 0.16f, y0 + s * 0.3f}, {x1 + s * 0.02f, y0 + s * 0.3f}, {x1 - s * 0.12f, y1}, {x0, y1}};
  dl->AddConvexPolyFilled(front, 4, c);
}

// A small 2600-style sprite: the graphics editor's files.
static void sprite(ImDrawList* dl, ImVec2 p, float s, ImU32 c) {
  static const unsigned char rows[7] = {0x08, 0x1C, 0x2A, 0x7F, 0x3E, 0x14, 0x22};
  float px = s / 8.0f, ox = p.x + s * 0.5f - px * 3.5f, oy = p.y + s * 0.5f - px * 3.5f;
  for (int y = 0; y < 7; y++)
    for (int x = 0; x < 7; x++)
      if (rows[y] & (0x40 >> x))
        dl->AddRectFilled(ImVec2(ox + x * px, oy + y * px), ImVec2(ox + (x + 1) * px - 0.5f, oy + (y + 1) * px - 0.5f), c);
}

// A cartridge: the label on top, the grip ridges below.
static void cartridge(ImDrawList* dl, ImVec2 p, float s, ImU32 c) {
  float x0 = p.x + s * 0.16f, x1 = p.x + s * 0.84f, y0 = p.y + s * 0.08f, y1 = p.y + s * 0.92f;
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), c, s * 0.08f);
  dl->AddRectFilled(ImVec2(x0 + s * 0.1f, y0 + s * 0.1f), ImVec2(x1 - s * 0.1f, y0 + s * 0.42f), IM_COL32(0, 0, 0, 110), s * 0.04f);
  float t = std::max(1.0f, s * 0.06f);
  for (int i = 0; i < 3; i++) {
    float y = y0 + s * (0.56f + i * 0.1f);
    dl->AddLine(ImVec2(x0 + s * 0.1f, y), ImVec2(x1 - s * 0.1f, y), IM_COL32(0, 0, 0, 110), t);
  }
}

// A joystick: the project.
static void joystick(ImDrawList* dl, ImVec2 p, float s, ImU32 c) {
  float cx = p.x + s * 0.55f;
  dl->AddRectFilled(ImVec2(p.x + s * 0.1f, p.y + s * 0.62f), ImVec2(p.x + s * 0.9f, p.y + s * 0.92f), c, s * 0.08f);
  dl->AddLine(ImVec2(cx, p.y + s * 0.64f), ImVec2(cx, p.y + s * 0.3f), c, std::max(1.5f, s * 0.12f));
  dl->AddCircleFilled(ImVec2(cx, p.y + s * 0.24f), s * 0.16f, c);
  dl->AddCircleFilled(ImVec2(p.x + s * 0.25f, p.y + s * 0.56f), s * 0.09f, theme::col(theme::Role::MsgError));
}

// A picture: a frame, a hill and the sun.
static void picture(ImDrawList* dl, ImVec2 p, float s, ImU32 c) {
  float x0 = p.x + s * 0.08f, x1 = p.x + s * 0.92f, y0 = p.y + s * 0.16f, y1 = p.y + s * 0.84f;
  dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), c, s * 0.06f, 0, std::max(1.0f, s * 0.08f));
  dl->AddTriangleFilled(ImVec2(x0 + s * 0.08f, y1 - s * 0.06f), ImVec2(x0 + s * 0.36f, y0 + s * 0.3f),
                        ImVec2(x0 + s * 0.62f, y1 - s * 0.06f), c);
  dl->AddTriangleFilled(ImVec2(x0 + s * 0.4f, y1 - s * 0.06f), ImVec2(x0 + s * 0.58f, y0 + s * 0.42f),
                        ImVec2(x1 - s * 0.06f, y1 - s * 0.06f), withAlpha(c, 0.7f));
  dl->AddCircleFilled(ImVec2(x1 - s * 0.2f, y0 + s * 0.18f), s * 0.07f, c);
}

void drawIcon(ImDrawList* dl, ImVec2 p, float s, Kind k, bool open) {
  using theme::Role;
  using theme::col;
  switch (k) {
    case Kind::Folder: folder(dl, p, s, col(Role::FilesFolder), open); break;
    case Kind::Project: joystick(dl, p, s, col(Role::FilesProject)); break;
    case Kind::Asm: page(dl, p, s, col(Role::FilesAsm), "A"); break;
    case Kind::C: page(dl, p, s, col(Role::FilesC), "C"); break;
    case Kind::Header: page(dl, p, s, col(Role::FilesHeader), "H"); break;
    case Kind::Basic: page(dl, p, s, col(Role::FilesBasic), "B"); break;
    case Kind::Graphics: sprite(dl, p, s, col(Role::FilesGraphics)); break;
    case Kind::Rom: cartridge(dl, p, s, col(Role::FilesRom)); break;
    case Kind::Image: picture(dl, p, s, col(Role::FilesImage)); break;
    case Kind::Listing: page(dl, p, s, col(Role::FilesListing), "L"); break;
    case Kind::Text: page(dl, p, s, col(Role::FilesText), nullptr); break;
  }
}

}  // namespace vcs::files
