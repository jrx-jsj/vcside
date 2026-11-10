// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "file_icons.h"

#include <algorithm>
#include <cctype>

#include "imgui.h"
#include "theme_ui.h"

namespace vcs::files {

static std::string lowerCase(std::string text) {
  for (auto& character : text) character = (char)std::tolower((unsigned char)character);
  return text;
}

Kind kindOf(const std::string& filename, bool directory) {
  if (directory) return Kind::Folder;
  std::string lowerName = lowerCase(filename);
  size_t dot = lowerName.rfind('.');
  std::string ext = dot == std::string::npos ? std::string() : lowerName.substr(dot);
  if (ext == ".vcsproj") return Kind::Project;
  if (ext == ".asm" || ext == ".s" || ext == ".a65" || ext == ".dasm") return Kind::Asm;
  if (ext == ".c") return Kind::C;
  if (ext == ".h" || ext == ".inc" || ext == ".equ") return Kind::Header;
  if (ext == ".bas" || ext == ".bb") return Kind::Basic;
  if (ext == ".vcsgfx") return Kind::Graphics;
  if (ext == ".bin" || ext == ".a26" || ext == ".rom" || ext == ".elf" || ext == ".ace") return Kind::Rom;
  if (ext == ".png" || ext == ".bmp" || ext == ".gif" || ext == ".jpg" || ext == ".jpeg") return Kind::Image;
  if (ext == ".lst" || ext == ".sym" || ext == ".map" || ext == ".lbl") return Kind::Listing;
  return Kind::Text;
}

static ImU32 withAlpha(ImU32 colour, float alpha) {
  return (colour & ~IM_COL32_A_MASK) | ((ImU32)(((colour >> IM_COL32_A_SHIFT) & 255) * alpha) << IM_COL32_A_SHIFT);
}

// A sheet with its top right corner folded; a letter on it says the language.
static void page(ImDrawList* drawList, ImVec2 position, float size, ImU32 colour, const char* letter) {
  float x0 = position.x + size * 0.17f, x1 = position.x + size * 0.83f, y0 = position.y + size * 0.05f, y1 = position.y + size * 0.95f, fold = size * 0.26f;
  ImVec2 points[5] = {{x0, y0}, {x1 - fold, y0}, {x1, y0 + fold}, {x1, y1}, {x0, y1}};
  if (letter) {
    drawList->AddConvexPolyFilled(points, 5, colour);
    drawList->AddTriangleFilled(ImVec2(x1 - fold, y0), ImVec2(x1 - fold, y0 + fold), ImVec2(x1, y0 + fold), IM_COL32(0, 0, 0, 90));
    ImFont* font = ImGui::GetFont();
    float fontSize = size * 0.62f;
    ImVec2 textSize = font->CalcTextSizeA(fontSize, 100, 0, letter);
    float lum = (0.299f * (colour & 255) + 0.587f * ((colour >> 8) & 255) + 0.114f * ((colour >> 16) & 255)) / 255;  // ImU32 is ABGR
    ImU32 ink = lum > 0.55f ? IM_COL32(0, 0, 0, 210) : IM_COL32(255, 255, 255, 235);
    drawList->AddText(font, fontSize, ImVec2((x0 + x1 - textSize.x) * 0.5f, y1 - textSize.y - size * 0.04f), ink, letter);
  } else {  // plain text: an outline with lines
    float thickness = std::max(1.0f, size * 0.07f);
    drawList->AddPolyline(points, 5, colour, ImDrawFlags_Closed, thickness);
    drawList->AddLine(ImVec2(x1 - fold, y0), ImVec2(x1 - fold, y0 + fold), colour, thickness);
    drawList->AddLine(ImVec2(x1 - fold, y0 + fold), ImVec2(x1, y0 + fold), colour, thickness);
    for (int i = 0; i < 3; i++) {
      float y = y0 + size * (0.42f + i * 0.15f);
      drawList->AddLine(ImVec2(x0 + size * 0.12f, y), ImVec2(x1 - size * (i == 2 ? 0.25f : 0.12f), y), withAlpha(colour, 0.8f), thickness);
    }
  }
}

static void folder(ImDrawList* drawList, ImVec2 position, float size, ImU32 colour, bool open) {
  float x0 = position.x + size * 0.06f, x1 = position.x + size * 0.94f, y0 = position.y + size * 0.18f, y1 = position.y + size * 0.84f, radius = size * 0.08f;
  // the tab
  drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + size * 0.38f, y0 + size * 0.2f), colour, radius, ImDrawFlags_RoundCornersTop);
  if (!open) {
    drawList->AddRectFilled(ImVec2(x0, y0 + size * 0.12f), ImVec2(x1, y1), colour, radius);
    return;
  }
  // open: the back, then the front leaning towards the viewer
  drawList->AddRectFilled(ImVec2(x0, y0 + size * 0.12f), ImVec2(x1 - size * 0.06f, y1), withAlpha(colour, 0.55f), radius);
  ImVec2 front[4] = {{x0 + size * 0.16f, y0 + size * 0.3f}, {x1 + size * 0.02f, y0 + size * 0.3f}, {x1 - size * 0.12f, y1}, {x0, y1}};
  drawList->AddConvexPolyFilled(front, 4, colour);
}

// A small 2600-style sprite: the graphics editor's files.
static void sprite(ImDrawList* drawList, ImVec2 position, float size, ImU32 colour) {
  static const unsigned char rows[7] = {0x08, 0x1C, 0x2A, 0x7F, 0x3E, 0x14, 0x22};
  float cellSize = size / 8.0f, originX = position.x + size * 0.5f - cellSize * 3.5f, originY = position.y + size * 0.5f - cellSize * 3.5f;
  for (int y = 0; y < 7; y++)
    for (int x = 0; x < 7; x++)
      if (rows[y] & (0x40 >> x))
        drawList->AddRectFilled(ImVec2(originX + x * cellSize, originY + y * cellSize), ImVec2(originX + (x + 1) * cellSize - 0.5f, originY + (y + 1) * cellSize - 0.5f), colour);
}

// A cartridge: the label on top, the grip ridges below.
static void cartridge(ImDrawList* drawList, ImVec2 position, float size, ImU32 colour) {
  float x0 = position.x + size * 0.16f, x1 = position.x + size * 0.84f, y0 = position.y + size * 0.08f, y1 = position.y + size * 0.92f;
  drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), colour, size * 0.08f);
  drawList->AddRectFilled(ImVec2(x0 + size * 0.1f, y0 + size * 0.1f), ImVec2(x1 - size * 0.1f, y0 + size * 0.42f), IM_COL32(0, 0, 0, 110), size * 0.04f);
  float thickness = std::max(1.0f, size * 0.06f);
  for (int i = 0; i < 3; i++) {
    float y = y0 + size * (0.56f + i * 0.1f);
    drawList->AddLine(ImVec2(x0 + size * 0.1f, y), ImVec2(x1 - size * 0.1f, y), IM_COL32(0, 0, 0, 110), thickness);
  }
}

// A joystick: the project.
static void joystick(ImDrawList* drawList, ImVec2 position, float size, ImU32 colour) {
  float cx = position.x + size * 0.55f;
  drawList->AddRectFilled(ImVec2(position.x + size * 0.1f, position.y + size * 0.62f), ImVec2(position.x + size * 0.9f, position.y + size * 0.92f), colour, size * 0.08f);
  drawList->AddLine(ImVec2(cx, position.y + size * 0.64f), ImVec2(cx, position.y + size * 0.3f), colour, std::max(1.5f, size * 0.12f));
  drawList->AddCircleFilled(ImVec2(cx, position.y + size * 0.24f), size * 0.16f, colour);
  drawList->AddCircleFilled(ImVec2(position.x + size * 0.25f, position.y + size * 0.56f), size * 0.09f, theme::col(theme::Role::MsgError));
}

// A picture: a frame, a hill and the sun.
static void picture(ImDrawList* drawList, ImVec2 position, float size, ImU32 colour) {
  float x0 = position.x + size * 0.08f, x1 = position.x + size * 0.92f, y0 = position.y + size * 0.16f, y1 = position.y + size * 0.84f;
  drawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), colour, size * 0.06f, 0, std::max(1.0f, size * 0.08f));
  drawList->AddTriangleFilled(ImVec2(x0 + size * 0.08f, y1 - size * 0.06f), ImVec2(x0 + size * 0.36f, y0 + size * 0.3f),
                              ImVec2(x0 + size * 0.62f, y1 - size * 0.06f), colour);
  drawList->AddTriangleFilled(ImVec2(x0 + size * 0.4f, y1 - size * 0.06f), ImVec2(x0 + size * 0.58f, y0 + size * 0.42f),
                              ImVec2(x1 - size * 0.06f, y1 - size * 0.06f), withAlpha(colour, 0.7f));
  drawList->AddCircleFilled(ImVec2(x1 - size * 0.2f, y0 + size * 0.18f), size * 0.07f, colour);
}

void drawIcon(ImDrawList* drawList, ImVec2 position, float size, Kind kind, bool open) {
  using theme::Role;
  using theme::col;
  switch (kind) {
    case Kind::Folder: folder(drawList, position, size, col(Role::FilesFolder), open); break;
    case Kind::Project: joystick(drawList, position, size, col(Role::FilesProject)); break;
    case Kind::Asm: page(drawList, position, size, col(Role::FilesAsm), "A"); break;
    case Kind::C: page(drawList, position, size, col(Role::FilesC), "C"); break;
    case Kind::Header: page(drawList, position, size, col(Role::FilesHeader), "H"); break;
    case Kind::Basic: page(drawList, position, size, col(Role::FilesBasic), "B"); break;
    case Kind::Graphics: sprite(drawList, position, size, col(Role::FilesGraphics)); break;
    case Kind::Rom: cartridge(drawList, position, size, col(Role::FilesRom)); break;
    case Kind::Image: picture(drawList, position, size, col(Role::FilesImage)); break;
    case Kind::Listing: page(drawList, position, size, col(Role::FilesListing), "L"); break;
    case Kind::Text: page(drawList, position, size, col(Role::FilesText), nullptr); break;
  }
}

}  // namespace vcs::files
