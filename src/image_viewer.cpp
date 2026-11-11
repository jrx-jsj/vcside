// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "image_viewer.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

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
  std::string extension = fs::path(path).extension().string();
  for (auto& character : extension) character = (char)std::tolower((unsigned char)character);
  return extension;
}

// ".bmp", ".png" or "": by the extension, or for any other name by the first bytes (a screenshot saved
// without an extension is still a picture).
static std::string imageKind(const std::string& path) {
  std::string extension = lowerExt(path);
  if (extension == ".png" || extension == ".bmp") return extension;
  unsigned char head[18] = {};
  std::ifstream file(path, std::ios::binary);
  if (!file.read(reinterpret_cast<char*>(head), sizeof head)) return "";
  static const unsigned char pngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (std::equal(pngSignature, pngSignature + 8, head)) return ".png";
  // "BM", then at byte 14 the size of the header that follows: one of the sizes the BMP versions use
  unsigned headerSize = head[14] | head[15] << 8 | head[16] << 16 | (unsigned)head[17] << 24;
  bool knownHeader = headerSize == 12 || headerSize == 40 || headerSize == 52 || headerSize == 56 || headerSize == 108 || headerSize == 124;
  if (head[0] == 'B' && head[1] == 'M' && knownHeader) return ".bmp";
  return "";
}

bool ImageViewer::handles(const std::string& path) { return !imageKind(path).empty(); }

ImageViewer::~ImageViewer() {
  if (texture_) {
    GLuint texture = texture_;
    glDeleteTextures(1, &texture);
  }
}

// The file as RGBA32 pixels.
static SDL_Surface* readImage(const std::string& path) {
  SDL_Surface* surface = nullptr;
  if (imageKind(path) == ".bmp") {
    surface = SDL_LoadBMP(path.c_str());
  } else {
#if SDL_VERSION_ATLEAST(3, 4, 0)
    surface = SDL_LoadPNG(path.c_str());
#else
    int width = 0, height = 0, channels = 0;
    if (unsigned char* pixelData = stbi_load(path.c_str(), &width, &height, &channels, 4)) {
      if (SDL_Surface* view = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixelData, width * 4)) {
        surface = SDL_DuplicateSurface(view);
        SDL_DestroySurface(view);
      }
      stbi_image_free(pixelData);
    } else {
      SDL_SetError("%s", stbi_failure_reason());
    }
#endif
  }
  if (surface && surface->format != SDL_PIXELFORMAT_RGBA32) {
    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    surface = rgba;
  }
  return surface;
}

bool ImageViewer::load(const std::string& path, std::string* error) {
  SDL_Surface* surface = readImage(path);
  if (!surface) {
    if (error) *error = SDL_GetError();
    return false;
  }
  path_ = path;
  std::error_code ec;
  mtime_ = fs::last_write_time(path, ec);
  if (surface->w != width_ || surface->h != height_) zoom_ = 0;  // another size: fit it again
  width_ = surface->w, height_ = surface->h;
  pixels_.assign((size_t)width_ * height_, 0);
  for (int y = 0; y < height_; y++) std::memcpy(&pixels_[(size_t)y * width_], (const char*)surface->pixels + (size_t)y * surface->pitch, (size_t)width_ * 4);
  SDL_DestroySurface(surface);
  if (texture_) upload();
  return true;
}

void ImageViewer::upload() {
  GLuint texture = texture_;
  if (!texture) glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  // nearest: every pixel stays a sharp square at any zoom
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels_.data());
  texture_ = texture;
}

void ImageViewer::fit(float viewW, float viewH) {
  if (width_ <= 0 || height_ <= 0) return;
  float fitZoom = std::min(viewW / width_, viewH / height_) * 0.92f;
  zoom_ = fitZoom >= 1 ? std::floor(fitZoom) : std::max(fitZoom, 0.02f);  // whole multiples keep the pixels even
  panX_ = width_ * 0.5f, panY_ = height_ * 0.5f;
}

void ImageViewer::render() {
  // changed on disk (an export, a converter): read it again, keeping the view
  if (++checkTick_ >= 60) {
    checkTick_ = 0;
    std::error_code ec;
    auto modified = fs::last_write_time(path_, ec);
    if (!ec && modified != mtime_) load(path_, nullptr);
  }
  if (!texture_ && !pixels_.empty()) upload();

  ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + em(8), ImGui::GetCursorPosY() + em(4)));
  ImGui::TextUnformatted(F("{0} × {1} px", {std::to_string(width_), std::to_string(height_)}).c_str());
  ImGui::SameLine(0, em(16));
  bool fitNow = ImGui::Button(T("Fit"));
  ImGui::SameLine();
  bool actual = ImGui::Button("1:1");
  ImGui::SameLine(0, em(12));
  ImGui::TextDisabled("%d %%", (int)std::lround(zoom_ * 100));

  ImVec2 avail = ImGui::GetContentRegionAvail();
  float infoH = ImGui::GetTextLineHeightWithSpacing();
  ImVec2 size(std::max(avail.x, 1.0f), std::max(avail.y - infoH, 1.0f));
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  if (zoom_ <= 0 || fitNow) fit(size.x, size.y);
  if (actual) zoom_ = 1, panX_ = width_ * 0.5f, panY_ = height_ * 0.5f;

  ImGuiIO& io = ImGui::GetIO();
  ImVec2 centre(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
  // zoom around a point of the view: the image pixel under it stays put
  auto zoomAt = [&](ImVec2 at, float factor) {
    float ix = panX_ + (at.x - centre.x) / zoom_, iy = panY_ + (at.y - centre.y) / zoom_;
    zoom_ = std::clamp(zoom_ * factor, 0.02f, 64.0f);
    panX_ = ix - (at.x - centre.x) / zoom_, panY_ = iy - (at.y - centre.y) / zoom_;
  };
  if (hovered && io.MouseWheel != 0) zoomAt(io.MousePos, std::pow(1.25f, io.MouseWheel));
  for (int button : {ImGuiMouseButton_Left, ImGuiMouseButton_Middle})
    if (active && ImGui::IsMouseDragging(button, 3)) {
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
  panX_ = std::clamp(panX_, 0.0f, (float)width_), panY_ = std::clamp(panY_, 0.0f, (float)height_);

  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
  drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), theme::col(theme::Role::EdBackground));
  ImVec2 imageMin(centre.x - panX_ * zoom_, centre.y - panY_ * zoom_), imageMax(imageMin.x + width_ * zoom_, imageMin.y + height_ * zoom_);
  // a chequered backdrop where the image is transparent
  ImVec2 visibleMin(std::max(imageMin.x, origin.x), std::max(imageMin.y, origin.y)), visibleMax(std::min(imageMax.x, origin.x + size.x), std::min(imageMax.y, origin.y + size.y));
  float checkerSize = em(8);
  for (float y = visibleMin.y; y < visibleMax.y; y += checkerSize)
    for (float x = visibleMin.x; x < visibleMax.x; x += checkerSize) {
      bool dark = ((int)((x - imageMin.x) / checkerSize) + (int)((y - imageMin.y) / checkerSize)) & 1;
      drawList->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + checkerSize, visibleMax.x), std::min(y + checkerSize, visibleMax.y)),
                              dark ? IM_COL32(150, 150, 150, 255) : IM_COL32(200, 200, 200, 255));
    }
  if (texture_) drawList->AddImage((ImTextureID)(intptr_t)texture_, imageMin, imageMax);
  // a pixel grid once the pixels are big enough
  if (zoom_ >= 12) {
    ImU32 gridColour = IM_COL32(0, 0, 0, 50);
    int x0 = std::max(0, (int)((visibleMin.x - imageMin.x) / zoom_)), x1 = std::min(width_, (int)((visibleMax.x - imageMin.x) / zoom_) + 1);
    int y0 = std::max(0, (int)((visibleMin.y - imageMin.y) / zoom_)), y1 = std::min(height_, (int)((visibleMax.y - imageMin.y) / zoom_) + 1);
    for (int x = x0; x <= x1; x++) drawList->AddLine(ImVec2(imageMin.x + x * zoom_, visibleMin.y), ImVec2(imageMin.x + x * zoom_, visibleMax.y), gridColour);
    for (int y = y0; y <= y1; y++) drawList->AddLine(ImVec2(visibleMin.x, imageMin.y + y * zoom_), ImVec2(visibleMax.x, imageMin.y + y * zoom_), gridColour);
  }
  drawList->AddRect(ImVec2(imageMin.x - 1, imageMin.y - 1), ImVec2(imageMax.x + 1, imageMax.y + 1), theme::col(theme::Role::UiBorder));
  drawList->PopClipRect();

  // the pixel under the mouse
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + em(8));
  int px = (int)std::floor((io.MousePos.x - imageMin.x) / zoom_), py = (int)std::floor((io.MousePos.y - imageMin.y) / zoom_);
  if (hovered && px >= 0 && py >= 0 && px < width_ && py < height_) {
    uint32_t pixel = pixels_[(size_t)py * width_ + px];  // bytes R, G, B, A in memory
    unsigned red = pixel & 255, green = (pixel >> 8) & 255, blue = (pixel >> 16) & 255, alpha = pixel >> 24;
    char info[96];
    snprintf(info, sizeof info, "x %d  y %d   #%02X%02X%02X%s", px, py, red, green, blue, alpha < 255 ? (" a " + std::to_string(alpha)).c_str() : "");
    ImGui::ColorButton("##px", ImVec4(red / 255.0f, green / 255.0f, blue / 255.0f, 1), ImGuiColorEditFlags_NoTooltip,
                       ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ImGui::TextUnformatted(info);
  } else {
    ImGui::TextDisabled("%s", T("Click: zoom in · Alt+click or right button: zoom out · drag: move"));
  }
}

}  // namespace vcs
