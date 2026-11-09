// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The editor of a .vcsgfx file: its sprites (layers of TIA objects, frames, flicker
// phases) and playfields (rows with their own mode and colours).
#pragma once
#include <imgui.h>

#include <string>
#include <vector>

#include "gfx.h"

namespace vcs {

class GfxEditor {
 public:
  bool load(const std::string& path, std::string* error);
  // Writes the .vcsgfx and, for a kernel of one's own, the tables next to it
  // (<name>.gfx.asm) for the program to include. *generated: the file written, if any.
  // project: what the project's program draws with; the asset may choose another kernel.
  bool save(const std::string& path, const gfx::Profile& project, std::string* error, std::string* generated = nullptr);
  void render(gfx::Tv tv, const gfx::Profile& project);
  gfx::Profile profile(const gfx::Profile& project) const;  // the asset's kernel, or the project's
  bool dirty() const { return dirty_; }
  void undo();
  void redo();
  const std::string& message() const { return message_; }  // what the last import or copy did

  gfx::Asset& asset() { return asset_; }
  static gfx::Asset blank();  // one 8x8 sprite on P0

 private:
  void toolbar(const gfx::Profile& project);
  void groupField(std::string& group);
  void spriteView(gfx::Sprite& sprite, gfx::Tv tv);
  void playfieldView(gfx::Playfield& playfield, gfx::Tv tv);
  void animations(gfx::Sprite& sprite, gfx::Tv tv, int thumbBounds[4], float cellWidth, float cellHeight);
  void issues(const std::vector<gfx::Issue>& list);
  bool colourButton(const char* id, int colour, gfx::Tv tv, int* out, bool allowInherit, float size);
  void drawSprite(const gfx::Sprite& sprite, int frame, int phase, ImVec2 origin, float cellWidth, float cellHeight, gfx::Tv tv, int backgroundColour,
                  int highlightLayer, int bounds[4]);
  void changed() { changed_ = true; dirty_ = true; }
  void endFrame();

  gfx::Profile kernelProfile_;        // the kernel of this frame
  std::string path_;      // the file, for the regions of batari Basic programs
  gfx::Asset asset_, committed_;
  std::vector<gfx::Asset> undo_, redo_;
  bool dirty_ = false, changed_ = false;
  int item_ = 0;          // sprites first, then playfields
  int layer_ = 0, frame_ = 0;
  int anim_ = 0, step_ = 0;  // the animation shown under the frames and its selected step
  double animStart_ = 0;     // when the preview started playing it
  float zoom_ = 14;
  bool flicker_ = true, othersDim_ = true;
  int previewScale_ = 3;
  int paintValue_ = -1;   // while dragging: 1 set, 0 clear
  enum class Tool { Pen, Line, Rect, RectFilled, Ellipse, EllipseFilled, Fill };
  Tool tool_ = Tool::Pen;
  int shapeX_ = 0, shapeY_ = 0;  // where a line, rectangle or ellipse started (layer pixels)
  bool toolBar();         // the drawing tools; true if one was picked
  bool moving_ = false;
  ImVec2 moveFrom_;
  int moveDx_ = 0, moveDy_ = 0;
  std::string message_;
};

}  // namespace vcs
