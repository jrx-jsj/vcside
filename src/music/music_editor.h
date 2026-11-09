// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The editor of a .vcsmusic file: patterns as a tracker grid or a piano roll, the
// order they play in, the instruments, and how in tune every note is on the TIA.
#pragma once
#include <imgui.h>

#include <string>
#include <vector>

#include "music_audio.h"
#include "song.h"

namespace vcs::music {

class MusicEditor {
 public:
  ~MusicEditor();
  bool load(const std::string& path, std::string* error);
  bool save(const std::string& path, std::string* error);
  // tv: the project's TV standard ("AUTO" is played and tuned as NTSC).
  void render(const std::string& tv);
  bool dirty() const { return dirty_; }
  void undo();
  void redo();
  Song& song() { return song_; }
  const std::string& message() const { return message_; }

 private:
  void toolbar();
  void orderPanel();
  void instrumentPanel();
  void patternGrid();
  void pianoRoll();
  void instrumentEditor();
  void tuningPanel();
  void keyboard();
  void gridKeys();
  bool envelope(const char* id, std::vector<int>& values, int low, int high, int* loopStart, int minLength);

  void enterNote(int note);
  void preview(int note);
  void stopPreview();
  void play(bool fromCursor);
  Pattern& pattern();
  int patternIndex() const;
  ImU32 tuningColour(const NotePitch& pitch) const;
  void changed() { changed_ = true, dirty_ = true; }
  void endFrame();

  std::string path_, tv_ = "NTSC";
  double clock_ = 31399.5;
  Song song_, committed_;
  std::vector<Song> undo_, redo_;
  bool dirty_ = false, changed_ = false;
  int order_ = 0, row_ = 0, channel_ = 0, field_ = 0;  // the cursor: field 0 note, 1 instrument
  int instrument_ = 0, octave_ = 4, step_ = 1;
  bool roll_ = false, follow_ = true;
  bool muted_[kChannels] = {false, false};
  int previewKey_ = -1;    // ImGuiKey held for the note being previewed
  bool previewMouse_ = false;
  bool scrollToCursor_ = false;
  std::string message_;
};

// The one sound output all music editors share.
MusicAudio& musicAudio();

}  // namespace vcs::music
