// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Shows a PNG or BMP in a tab: sharp pixels, zoom by clicking (Alt or the right
// button zooms out) or with the wheel, pan by dragging, the pixel under the mouse.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vcs {

class ImageViewer {
 public:
  ~ImageViewer();
  static bool handles(const std::string& path);  // by extension
  bool load(const std::string& path, std::string* error);
  void render();

 private:
  void upload();
  void fit(float viewW, float viewH);

  std::string path_;
  std::filesystem::file_time_type mtime_{};
  int width_ = 0, height_ = 0;
  std::vector<uint32_t> pixels_;  // RGBA, row by row
  unsigned texture_ = 0;
  float zoom_ = 0;                // 0: fit it on the next frame
  float panX_ = 0, panY_ = 0;     // image pixel at the centre of the view
  bool dragging_ = false;
  int checkTick_ = 0;
};

}  // namespace vcs
