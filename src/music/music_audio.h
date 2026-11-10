// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The music editor's sound: a Renderer fed to the computer's speakers through SDL3. SDL
// asks for samples from its own thread; the editor's calls take a lock.
#pragma once
#include <mutex>
#include <string>

#include "player.h"

struct SDL_AudioStream;

namespace vcs::music {

class MusicAudio {
 public:
  ~MusicAudio();
  // Closes the output; call before SDL_Quit (afterwards SDL has already freed it).
  void close();
  // Opens the default output the first time; false (and why) if there is none.
  bool ready(std::string* error);

  void play(const Song& song, const std::string& tv, SongPosition from);
  void stop();
  void updateSong(const Song& song);
  void noteOn(const Song& song, const std::string& tv, int channel, int note, int instrument);
  void noteOff(int channel);
  void setMuted(int channel, bool muted);

  bool playing();
  SongPosition position();
  FrameRegisters registers();  // what the TIA is being given now

 private:
  void use(const std::string& tv);  // the renderer for this TV standard
  void fill(int bytesWanted);
  static void feed(void* userData, SDL_AudioStream* stream, int additionalBytes, int totalBytes);

  std::mutex mutex_;
  Renderer renderer_;
  std::string tv_;
  SDL_AudioStream* stream_ = nullptr;
  bool failed_ = false;
  std::string error_;
  std::vector<float> buffer_;
};

}  // namespace vcs::music
