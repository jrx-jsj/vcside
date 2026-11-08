// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "image_viewer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "i18n.h"
#include "imgui.h"
#include "theme_ui.h"

// SDL 3.4 reads PNG itself; older SDL 3 builds use stb_image (built in window_icon.cpp).
#if !SDL_VERSION_ATLEAST(3, 4, 0)
#include "stb_image.h"
#endif

namespace fs = std::filesystem;

namespace vcs {

using theme::em;

static std::string lowerExt(const std::string& path) {
  std::string e = fs::path(path).extension().string();
  for (auto& c : e) c = (char)std::tolower((unsigned char)c);
  return e;
}

bool ImageViewer::handles(const std::string& path) {
  std::string e = lowerExt(path);
  return e == ".png" || e == ".bmp";
}

ImageViewer::~ImageViewer() {
  if (texture_) {
    GLuint t = texture_;
    glDeleteTextures(1, &t);
  }
}

// The file as RGBA32 pixels.
static SDL_Surface* readImage(const std::string& path) {
  SDL_Surface* s = nullptr;
  if (lowerExt(path) == ".bmp") {
    s = SDL_LoadBMP(path.c_str());
  } else {
#if SDL_VERSION_ATLEAST(3, 4, 0)
    s = SDL_LoadPNG(path.c_str());
#else
    int w = 0, h = 0, n = 0;
    if (unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4)) {
      if (SDL_Surface* view = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4)) {
        s = SDL_DuplicateSurface(view);
        SDL_DestroySurface(view);
      }
      stbi_image_free(px);
    } else {
      SDL_SetError("%s", stbi_failure_reason());
    }
#endif
  }
  if (s && s->format != SDL_PIXELFORMAT_RGBA32) {
    SDL_Surface* rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    s = rgba;
  }
  return s;
}

bool ImageViewer::load(const std::string& path, std::string* error) {
  SDL_Surface* s = readImage(path);
  if (!s) {
    if (error) *error = SDL_GetError();
    return false;
  }
  path_ = path;
  std::error_code ec;
  mtime_ = fs::last_write_time(path, ec);
  if (s->w != w_ || s->h != h_) zoom_ = 0;  // another size: fit it again
  w_ = s->w, h_ = s->h;
  pixels_.assign((size_t)w_ * h_, 0);
  for (int y = 0; y < h_; y++) std::memcpy(&pixels_[(size_t)y * w_], (const char*)s->pixels + (size_t)y * s->pitch, (size_t)w_ * 4);
  SDL_DestroySurface(s);
  if (texture_) upload();
  return true;
}

void ImageViewer::upload() {
  GLuint t = texture_;
  if (!t) glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  // nearest: every pixel stays a sharp square at any zoom
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w_, h_, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
  texture_ = t;
}

void ImageViewer::fit(float viewW, float viewH) {
  if (w_ <= 0 || h_ <= 0) return;
  float z = std::min(viewW / w_, viewH / h_) * 0.92f;
  zoom_ = z >= 1 ? std::floor(z) : std::max(z, 0.02f);  // whole multiples keep the pixels even
  panX_ = w_ * 0.5f, panY_ = h_ * 0.5f;
}

void ImageViewer::render() {
  // changed on disk (an export, a converter): read it again, keeping the view
  if (++checkTick_ >= 60) {
    checkTick_ = 0;
    std::error_code ec;
    auto t = fs::last_write_time(path_, ec);
    if (!ec && t != mtime_) load(path_, nullptr);
  }
  if (!texture_ && !pixels_.empty()) upload();

  ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + em(8), ImGui::GetCursorPosY() + em(4)));
  ImGui::TextUnformatted(F("{0} × {1} px", {std::to_string(w_), std::to_string(h_)}).c_str());
  ImGui::SameLine(0, em(16));
  bool fitNow = ImGui::Button(T("Fit"));
  ImGui::SameLine();
  bool actual = ImGui::Button("1:1");
  ImGui::SameLine(0, em(12));
  ImGui::TextDisabled("%d %%", (int)std::lround(zoom_ * 100));

  ImVec2 avail = ImGui::GetContentRegionAvail();
  float infoH = ImGui::GetTextLineHeightWithSpacing();
  ImVec2 size(std::max(avail.x, 1.0f), std::max(avail.y - infoH, 1.0f));
  ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  if (zoom_ <= 0 || fitNow) fit(size.x, size.y);
  if (actual) zoom_ = 1, panX_ = w_ * 0.5f, panY_ = h_ * 0.5f;

  ImGuiIO& io = ImGui::GetIO();
  ImVec2 centre(o.x + size.x * 0.5f, o.y + size.y * 0.5f);
  // zoom around a point of the view: the image pixel under it stays put
  auto zoomAt = [&](ImVec2 at, float factor) {
    float ix = panX_ + (at.x - centre.x) / zoom_, iy = panY_ + (at.y - centre.y) / zoom_;
    zoom_ = std::clamp(zoom_ * factor, 0.02f, 64.0f);
    panX_ = ix - (at.x - centre.x) / zoom_, panY_ = iy - (at.y - centre.y) / zoom_;
  };
  if (hovered && io.MouseWheel != 0) zoomAt(io.MousePos, std::pow(1.25f, io.MouseWheel));
  for (int b : {ImGuiMouseButton_Left, ImGuiMouseButton_Middle})
    if (active && ImGui::IsMouseDragging(b, 3)) {
      panX_ -= io.MouseDelta.x / zoom_, panY_ -= io.MouseDelta.y / zoom_;
      dragging_ = true;
    }
  if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    if (!dragging_) zoomAt(io.MousePos, io.KeyAlt ? 0.5f : 2.0f);
  } else if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    zoomAt(io.MousePos, 0.5f);
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
      !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    dragging_ = false;
  if (hovered) ImGui::SetMouseCursor(dragging_ ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
  // keep part of the image in view
  panX_ = std::clamp(panX_, 0.0f, (float)w_), panY_ = std::clamp(panY_, 0.0f, (float)h_);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(o, ImVec2(o.x + size.x, o.y + size.y), true);
  dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), theme::col(theme::Role::EdBackground));
  ImVec2 p0(centre.x - panX_ * zoom_, centre.y - panY_ * zoom_), p1(p0.x + w_ * zoom_, p0.y + h_ * zoom_);
  // a chequered backdrop where the image is transparent
  ImVec2 c0(std::max(p0.x, o.x), std::max(p0.y, o.y)), c1(std::min(p1.x, o.x + size.x), std::min(p1.y, o.y + size.y));
  float sq = em(8);
  for (float y = c0.y; y < c1.y; y += sq)
    for (float x = c0.x; x < c1.x; x += sq) {
      bool dark = ((int)((x - p0.x) / sq) + (int)((y - p0.y) / sq)) & 1;
      dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + sq, c1.x), std::min(y + sq, c1.y)),
                        dark ? IM_COL32(150, 150, 150, 255) : IM_COL32(200, 200, 200, 255));
    }
  if (texture_) dl->AddImage((ImTextureID)(intptr_t)texture_, p0, p1);
  // a pixel grid once the pixels are big enough
  if (zoom_ >= 12) {
    ImU32 g = IM_COL32(0, 0, 0, 50);
    int x0 = std::max(0, (int)((c0.x - p0.x) / zoom_)), x1 = std::min(w_, (int)((c1.x - p0.x) / zoom_) + 1);
    int y0 = std::max(0, (int)((c0.y - p0.y) / zoom_)), y1 = std::min(h_, (int)((c1.y - p0.y) / zoom_) + 1);
    for (int x = x0; x <= x1; x++) dl->AddLine(ImVec2(p0.x + x * zoom_, c0.y), ImVec2(p0.x + x * zoom_, c1.y), g);
    for (int y = y0; y <= y1; y++) dl->AddLine(ImVec2(c0.x, p0.y + y * zoom_), ImVec2(c1.x, p0.y + y * zoom_), g);
  }
  dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), theme::col(theme::Role::UiBorder));
  dl->PopClipRect();

  // the pixel under the mouse
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + em(8));
  int px = (int)std::floor((io.MousePos.x - p0.x) / zoom_), py = (int)std::floor((io.MousePos.y - p0.y) / zoom_);
  if (hovered && px >= 0 && py >= 0 && px < w_ && py < h_) {
    uint32_t c = pixels_[(size_t)py * w_ + px];  // bytes R, G, B, A in memory
    unsigned r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
    char info[96];
    snprintf(info, sizeof info, "x %d  y %d   #%02X%02X%02X%s", px, py, r, g, b, a < 255 ? (" a " + std::to_string(a)).c_str() : "");
    ImGui::ColorButton("##px", ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1), ImGuiColorEditFlags_NoTooltip,
                       ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ImGui::TextUnformatted(info);
  } else {
    ImGui::TextDisabled("%s", T("Click: zoom in · Alt+click or right button: zoom out · drag: move"));
  }
}

}  // namespace vcs
