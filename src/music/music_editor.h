// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The editor of a .vcsmusic file: patterns as a tracker grid or a piano roll, the
// order they play in, the instruments, and how in tune every note is on the TIA.
#pragma once
#include <imgui.h>

#include <string>
#include <vector>

#include "export.h"
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
  void registerView();
  void noteTuningEditor(Instrument& instrument);
  std::string explain(const VoiceTrace& trace) const;
  void instrumentEditor();
  void tuningPanel();
  void effectsPanel();
  void soundPanel();
  void waveEditor(Instrument& instrument);
  bool drum(const Instrument& instrument) const { return !song_.samples() && instrument.drum; }
  void exportPopup();
  // Writes <name>.music.asm (or .music.h for C) as the song says (Song::exportTo); quiet: only problems are said.
  bool exportSongFile(bool quiet);
  ExportTarget exportTarget() const;  // Song::exportTo, or what the song's sound is exported as by default
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
  void changed() { changed_ = true, dirty_ = true, revision_++; }
  void endFrame();

  std::string path_, tv_ = "NTSC";
  double clock_ = 31399.5;
  Song song_, committed_;
  std::vector<Song> undo_, redo_;
  bool dirty_ = false, changed_ = false;
  int revision_ = 0;  // goes up with every change, for what is worked out from the song
  int previewChannel_ = 0;  // where the note being previewed sounds (kTiaChannel: the TIA's channel 1)
  int order_ = 0, row_ = 0, channel_ = 0, field_ = 0;  // the cursor: field 0 note, 1 instrument, 2 effect
  int instrument_ = 0, octave_ = 4, step_ = 1;
  enum class View { Tracker, PianoRoll, Registers };
  View view_ = View::Tracker;
  bool follow_ = true;
  // the register view: the frames of the current order entry, played from the song's start
  struct TracedFrame {
    SongPosition position;
    FrameRegisters registers;
    VoiceTrace voices[kMaxChannels];
  };
  std::vector<TracedFrame> traced_;
  FrameRegisters tracedBefore_;  // the frame before the first one, to see what changes
  int tracedRevision_ = -1, tracedOrder_ = -1;
  double tracedClock_ = 0;
  int volumeVariant_ = 0, releaseVariant_ = 0;  // shown in the instrument editor
  int pitchRange_ = 100;  // samples: the pitch envelope's bars go from -this to +this cents
  int handNote_ = -1;                           // the note open in the tuning by hand
  bool muted_[kMaxChannels] = {};
  int previewKey_ = -1;    // ImGuiKey held for the note being previewed
  bool previewMouse_ = false;
  bool scrollToCursor_ = false;
  std::string message_;
  std::vector<std::string> exportProblems_;
  int effect_ = 0;  // the sound effect shown
};

// The one sound output all music editors share.
MusicAudio& musicAudio();

}  // namespace vcs::music
