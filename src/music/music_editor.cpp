// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "music_editor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "../i18n.h"
#include "../theme_ui.h"
#include "imgui_stdlib.h"

namespace fs = std::filesystem;

namespace vcs::music {

using theme::em;

MusicAudio& musicAudio() {
  static MusicAudio audio;
  return audio;
}

MusicEditor::~MusicEditor() {
  // the song this editor plays stops with it
  musicAudio().stop();
  if (previewKey_ >= 0 || previewMouse_) musicAudio().noteOff(channel_);
}

bool MusicEditor::load(const std::string& path, std::string* error) {
  path_ = path;
  std::error_code errorCode;
  if (!fs::exists(path, errorCode) || fs::file_size(path, errorCode) == 0) {
    song_ = blankSong();
  } else {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    if (!fromJson(text.str(), &song_, error)) return false;
  }
  if (song_.instruments.empty()) song_.instruments = blankSong().instruments;
  committed_ = song_;
  undo_.clear(), redo_.clear();
  dirty_ = changed_ = false;
  revision_++;
  return true;
}

bool MusicEditor::save(const std::string& path, std::string* error) {
  std::ofstream file(path, std::ios::binary);
  if (!file || !(file << toJson(song_))) {
    if (error) *error = F("Cannot write {0}", {path});
    return false;
  }
  path_ = path;
  dirty_ = false;
  if (!song_.exportTo.empty()) exportSongFile(true);  // kept up to date
  return true;
}

ExportTarget MusicEditor::exportTarget() const {
  ExportTarget target = song_.samples() ? ExportTarget::C : ExportTarget::Dasm;
  exportTargetOf(song_.exportTo, &target);
  return target;
}

static std::string exportPath(const std::string& songPath, ExportTarget target) {
  fs::path path(songPath);
  return (path.parent_path() / (path.stem().string() + exportExtension(target))).string();
}

bool MusicEditor::exportSongFile(bool quiet) {
  ExportOptions options;
  options.target = exportTarget();
  options.ramStart = options.target == ExportTarget::BatariBasic ? song_.exportRam : "";
  options.tv = tv_;
  ExportResult result = exportSong(song_, audioClockHz(tv_), options);
  exportProblems_ = result.problems;
  std::string path = exportPath(path_, options.target);
  if (!result.ok) {
    message_ = F("Not exported: {0}", {result.problems.empty() ? std::string() : result.problems[0]});
    return false;
  }
  std::ofstream file(path, std::ios::binary);
  if (!file || !(file << result.text)) {
    message_ = F("Cannot write {0}", {path});
    return false;
  }
  if (!quiet || !result.problems.empty())
    message_ = F("Exported {0}: {1} bytes of song, {2} sounds{3}", {fs::path(path).filename().string(), std::to_string(result.dataBytes), std::to_string(result.sounds),
                                                                    result.problems.empty() ? std::string() : F("; {0} warnings", {std::to_string(result.problems.size())})});
  return true;
}

void MusicEditor::undo() {
  if (undo_.empty()) return;
  redo_.push_back(song_);
  song_ = undo_.back();
  undo_.pop_back();
  committed_ = song_;
  dirty_ = true, revision_++;
  musicAudio().updateSong(song_);
}

void MusicEditor::redo() {
  if (redo_.empty()) return;
  undo_.push_back(song_);
  song_ = redo_.back();
  redo_.pop_back();
  committed_ = song_;
  dirty_ = true, revision_++;
  musicAudio().updateSong(song_);
}

// A change is one undo step once the mouse is up and no field is being edited; the
// song playing takes it at once.
void MusicEditor::endFrame() {
  if (!changed_) return;
  musicAudio().updateSong(song_);
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsAnyItemActive()) return;
  undo_.push_back(committed_);
  if (undo_.size() > 200) undo_.erase(undo_.begin());
  redo_.clear();
  committed_ = song_;
  changed_ = false;
}

int MusicEditor::patternIndex() const {
  if (song_.order.empty()) return 0;
  return std::clamp(song_.order[std::clamp(order_, 0, (int)song_.order.size() - 1)], 0, (int)song_.patterns.size() - 1);
}

Pattern& MusicEditor::pattern() { return song_.patterns[patternIndex()]; }

// How far off a note is: in tune (within 10 cents), a little (25), clearly, or not playable.
ImU32 MusicEditor::tuningColour(const NotePitch& pitch) const {
  if (!pitch.playable) return theme::col(theme::Role::MsgError);
  double off = std::fabs(pitch.cents);
  if (off <= 10) return theme::col(theme::Role::SpaceOk);
  if (off <= 25) return theme::col(theme::Role::SpaceWarning);
  return theme::col(theme::Role::SpaceFull);
}

static std::string cellNote(int note) {
  if (note == kNoteOff) return "===";
  if (note < 0) return "---";
  static const char* names[] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
  return std::string(names[note % 12]) + std::to_string(note / 12 - 1);
}

void MusicEditor::preview(int note) {
  if (instrument_ < 0 || instrument_ >= (int)song_.instruments.size()) return;
  musicAudio().noteOn(song_, tv_, channel_, note, instrument_);
}

void MusicEditor::stopPreview() { musicAudio().noteOff(channel_); }

void MusicEditor::play(bool fromCursor) {
  std::string error;
  if (!musicAudio().ready(&error)) {
    message_ = F("No sound output: {0}", {error});
    return;
  }
  musicAudio().play(song_, tv_, fromCursor ? SongPosition{order_, row_} : SongPosition{});
}

void MusicEditor::render(const std::string& tv) {
  tv_ = tv == "AUTO" || tv.empty() ? "NTSC" : tv;
  clock_ = audioClockHz(tv_);
  ImGuiIO& io = ImGui::GetIO();
  bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
  if (focused && io.KeyCtrl && !ImGui::IsAnyItemActive()) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? redo() : undo();
    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
  }
  if (song_.patterns.empty()) song_ = blankSong();
  order_ = std::clamp(order_, 0, (int)song_.order.size() - 1);
  instrument_ = std::clamp(instrument_, 0, std::max(0, (int)song_.instruments.size() - 1));
  if (follow_ && musicAudio().playing()) {
    SongPosition position = musicAudio().position();
    if (position.order != order_ || position.row != row_) scrollToCursor_ = true;
    order_ = std::clamp(position.order, 0, (int)song_.order.size() - 1), row_ = position.row;
  }
  row_ = std::clamp(row_, 0, pattern().rows - 1);

  ImGui::SetCursorPos(ImVec2(em(8), em(6)));
  ImGui::BeginGroup();
  toolbar();
  ImGui::EndGroup();
  ImGui::Separator();
  float side = em(190), right = em(330);
  float height = ImGui::GetContentRegionAvail().y;
  ImGui::BeginChild("##left", ImVec2(side, height), ImGuiChildFlags_Borders);
  soundPanel();
  orderPanel();
  ImGui::Spacing();
  instrumentPanel();
  ImGui::EndChild();
  ImGui::SameLine();
  float centre = std::max(em(200), ImGui::GetContentRegionAvail().x - right - ImGui::GetStyle().ItemSpacing.x);
  ImGui::BeginChild("##centre", ImVec2(centre, height), ImGuiChildFlags_None);
  float keysHeight = em(70);
  static const char* viewIds[] = {"##grid", "##roll", "##registers"};
  ImGui::BeginChild(viewIds[(int)view_], ImVec2(0, ImGui::GetContentRegionAvail().y - keysHeight - ImGui::GetStyle().ItemSpacing.y), ImGuiChildFlags_Borders,
                    view_ == View::PianoRoll ? ImGuiWindowFlags_HorizontalScrollbar : 0);
  if (view_ == View::PianoRoll) pianoRoll();
  else if (view_ == View::Registers) registerView();
  else patternGrid();
  ImGui::EndChild();
  keyboard();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##right", ImVec2(0, height), ImGuiChildFlags_Borders);
  if (ImGui::BeginTabBar("##sidetabs")) {
    if (ImGui::BeginTabItem(T("Instrument"))) {
      instrumentEditor();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Tuning"))) {
      tuningPanel();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Sound effects"))) {
      effectsPanel();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();
  if (previewMouse_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) previewMouse_ = false, stopPreview();
  endFrame();
}

// ---------------------------------------------------------------------------

void MusicEditor::toolbar() {
  bool playing = musicAudio().playing();
  if (ImGui::Button(playing ? T("Stop") : T("Play"))) playing ? musicAudio().stop() : play(false);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The whole song from the start. Space in the pattern plays from the cursor."));
  ImGui::SameLine();
  if (ImGui::Button(T("From the cursor"))) play(true);
  ImGui::SameLine();
  ImGui::Checkbox(T("Follow"), &follow_);
  for (int channel = 0; channel < song_.channelCount(); channel++) {
    ImGui::SameLine();
    bool heard = !muted_[channel];
    std::string label = F("Channel {0}", {std::to_string(channel)});
    if (ImGui::Checkbox(label.c_str(), &heard)) muted_[channel] = !heard, musicAudio().setMuted(channel, muted_[channel]);
  }
  ImGui::SameLine(0, em(20));
  if (ImGui::RadioButton(T("Tracker"), view_ == View::Tracker)) view_ = View::Tracker;
  ImGui::SameLine();
  if (ImGui::RadioButton(T("Piano roll"), view_ == View::PianoRoll)) view_ = View::PianoRoll;
  ImGui::SameLine();
  if (ImGui::RadioButton(T("Registers"), view_ == View::Registers)) view_ = View::Registers;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("What the player writes to AUDC, AUDF and AUDV on every video frame of this pattern."));
  // second line: timing, typing and files
  ImGui::SetNextItemWidth(em(90));
  int speed = song_.speed;
  if (ImGui::InputInt(T("Speed"), &speed)) song_.speed = std::clamp(speed, 1, 60), changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Video frames per row."));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(em(90));
  int swing = song_.swing;
  if (ImGui::InputInt(T("Swing"), &swing)) song_.swing = std::clamp(swing, 0, 60), changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Frames of the odd rows, for a swing (0: the same as the speed). As in TIATracker."));
  ImGui::SameLine(0, em(20));
  ImGui::SetNextItemWidth(em(90));
  if (ImGui::InputInt(T("Octave"), &octave_)) octave_ = std::clamp(octave_, 0, 8);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(em(90));
  if (ImGui::InputInt(T("Step"), &step_)) step_ = std::clamp(step_, 0, 16);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Rows the cursor moves after a note is typed."));
  ImGui::SameLine(0, em(20));
  if (ImGui::Button(T("Save as WAV"))) {
    std::string wavPath = fs::path(path_).replace_extension(".wav").string(), error;
    auto samples = renderSong(song_, tv_, 48000, 60 * 60 * 10);
    message_ = writeWav(wavPath, samples, 48000, &error) ? F("Written {0} ({1} s)", {fs::path(wavPath).filename().string(), std::to_string(samples.size() / 48000)}) : error;
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The song once through, next to this file, as the TIA plays it."));
  ImGui::SameLine();
  if (ImGui::Button(T("Export..."))) ImGui::OpenPopup("##export");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", F("{0}: the song and VCSIDE's player for the console, kept up to date on every save.", {fs::path(exportPath(path_, exportTarget())).filename().string()}).c_str());
  exportPopup();
  if (!message_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", message_.c_str());
  }
}

void MusicEditor::orderPanel() {
  ImGui::SeparatorText(T("Order"));
  ImGui::BeginChild("##order", ImVec2(0, em(150)), ImGuiChildFlags_Borders);
  for (int entry = 0; entry < (int)song_.order.size(); entry++) {
    int index = std::clamp(song_.order[entry], 0, (int)song_.patterns.size() - 1);
    std::string name = song_.patterns[index].name.empty() ? F("pattern {0}", {std::to_string(index)}) : song_.patterns[index].name;
    std::string label = (entry == song_.loopTo ? "↻ " : "   ") + std::to_string(entry) + "  " + name + "##o" + std::to_string(entry);
    if (ImGui::Selectable(label.c_str(), entry == order_)) order_ = entry, row_ = 0;
  }
  ImGui::EndChild();
  auto removeUnusedPatterns = [this] {
    for (int index = (int)song_.patterns.size() - 1; index >= 0 && song_.patterns.size() > 1; index--) {
      if (std::find(song_.order.begin(), song_.order.end(), index) != song_.order.end()) continue;
      song_.patterns.erase(song_.patterns.begin() + index);
      for (int& entry : song_.order)
        if (entry > index) entry--;
    }
  };
  if (ImGui::SmallButton(T("New"))) {
    Pattern fresh;
    fresh.resize(pattern().rows);
    song_.patterns.push_back(fresh);
    song_.order.insert(song_.order.begin() + order_ + 1, (int)song_.patterns.size() - 1);
    order_++, row_ = 0, changed();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("A new empty pattern after this entry."));
  ImGui::SameLine();
  if (ImGui::SmallButton(T("Copy"))) {
    Pattern copy = pattern();
    copy.name += copy.name.empty() ? "" : "'";
    song_.patterns.push_back(copy);
    song_.order.insert(song_.order.begin() + order_ + 1, (int)song_.patterns.size() - 1);
    order_++, changed();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("A copy of this pattern, to change it without changing the original."));
  ImGui::SameLine();
  if (ImGui::SmallButton(T("Again"))) {
    song_.order.insert(song_.order.begin() + order_ + 1, song_.order[order_]);
    order_++, changed();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Play the same pattern again: editing one changes both."));
  if (ImGui::ArrowButton("##up", ImGuiDir_Up) && order_ > 0) std::swap(song_.order[order_], song_.order[order_ - 1]), order_--, changed();
  ImGui::SameLine();
  if (ImGui::ArrowButton("##down", ImGuiDir_Down) && order_ + 1 < (int)song_.order.size()) std::swap(song_.order[order_], song_.order[order_ + 1]), order_++, changed();
  ImGui::SameLine();
  if (ImGui::SmallButton(T("Remove")) && song_.order.size() > 1) {
    song_.order.erase(song_.order.begin() + order_);
    if (song_.loopTo >= (int)song_.order.size()) song_.loopTo = (int)song_.order.size() - 1;
    order_ = std::min(order_, (int)song_.order.size() - 1);
    removeUnusedPatterns();
    changed();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(T("Loop here"))) song_.loopTo = song_.loopTo == order_ ? -1 : order_, changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("At the end the song goes back to this entry (↻). Again: it stops at the end."));
  Pattern& current = pattern();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputTextWithHint("##pname", T("pattern name"), &current.name)) changed();
  ImGui::SetNextItemWidth(em(80));
  int rows = current.rows;
  if (ImGui::InputInt(T("Rows"), &rows, 1, 8, ImGuiInputTextFlags_EnterReturnsTrue)) current.resize(rows), changed();
  // a speed of its own while it plays (0: the song's)
  ImGui::SetNextItemWidth(em(80));
  int speed = current.speed;
  if (ImGui::InputInt(T("Pattern speed"), &speed)) current.speed = std::clamp(speed, 0, 60), current.swing = current.speed ? current.swing : 0, changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Frames per row while this pattern plays. 0: the song's speed and swing."));
  if (current.speed > 0) {
    ImGui::SetNextItemWidth(em(80));
    int swing = current.swing;
    if (ImGui::InputInt(T("Pattern swing"), &swing)) current.swing = std::clamp(swing, 0, 60), changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Frames of this pattern's odd rows. 0: the same as the even ones."));
  }
}

void MusicEditor::instrumentPanel() {
  ImGui::SeparatorText(T("Instruments"));
  ImGui::BeginChild("##instruments", ImVec2(0, em(140)), ImGuiChildFlags_Borders);
  for (int index = 0; index < (int)song_.instruments.size(); index++) {
    char label[96];
    snprintf(label, sizeof label, "%02X  %s##i%d", index, song_.instruments[index].name.c_str(), index);
    if (ImGui::Selectable(label, index == instrument_)) instrument_ = index;
    if (drum(song_.instruments[index])) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", T("drum"));
    }
  }
  ImGui::EndChild();
  ImGui::PushID("instrumentButtons");
  if (ImGui::SmallButton(T("New"))) {
    song_.instruments.push_back(Instrument());
    instrument_ = (int)song_.instruments.size() - 1, changed();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(T("Copy")) && !song_.instruments.empty()) {
    Instrument copy = song_.instruments[instrument_];
    copy.name += "'";
    song_.instruments.push_back(copy);
    instrument_ = (int)song_.instruments.size() - 1, changed();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(T("Remove")) && song_.instruments.size() > 1) {
    // the cells that used it become "the channel's last instrument"; later ones move down
    for (auto& songPattern : song_.patterns)
      for (auto& channel : songPattern.cells)
        for (auto& cell : channel) {
          if (cell.instrument == instrument_) cell.instrument = -1;
          else if (cell.instrument > instrument_) cell.instrument--;
        }
    song_.instruments.erase(song_.instruments.begin() + instrument_);
    instrument_ = std::min(instrument_, (int)song_.instruments.size() - 1), changed();
  }
  ImGui::PopID();
}

// ---------------------------------------------------------------------------
// The tracker grid

// Note keys by their place on a US keyboard, two octaves as on a piano: Z-M the lower,
// Q-U the upper (the black keys on the row above).
static const std::pair<ImGuiKey, int> kNoteKeys[] = {
    {ImGuiKey_Z, 0},  {ImGuiKey_S, 1},  {ImGuiKey_X, 2},  {ImGuiKey_D, 3},  {ImGuiKey_C, 4},     {ImGuiKey_V, 5},      {ImGuiKey_G, 6},
    {ImGuiKey_B, 7},  {ImGuiKey_H, 8},  {ImGuiKey_N, 9},  {ImGuiKey_J, 10}, {ImGuiKey_M, 11},    {ImGuiKey_Comma, 12}, {ImGuiKey_L, 13},
    {ImGuiKey_Period, 14}, {ImGuiKey_Semicolon, 15}, {ImGuiKey_Slash, 16},
    {ImGuiKey_Q, 12}, {ImGuiKey_2, 13}, {ImGuiKey_W, 14}, {ImGuiKey_3, 15}, {ImGuiKey_E, 16},    {ImGuiKey_R, 17},     {ImGuiKey_5, 18},
    {ImGuiKey_T, 19}, {ImGuiKey_6, 20}, {ImGuiKey_Y, 21}, {ImGuiKey_7, 22}, {ImGuiKey_U, 23},    {ImGuiKey_I, 24},     {ImGuiKey_9, 25},
    {ImGuiKey_O, 26}, {ImGuiKey_0, 27}, {ImGuiKey_P, 28}};

void MusicEditor::enterNote(int note) {
  Cell& cell = pattern().cells[channel_][row_];
  cell.note = note;
  if (note >= 0) cell.instrument = instrument_;
  changed();
  row_ = std::min(row_ + step_, pattern().rows - 1);
  scrollToCursor_ = true;
}

void MusicEditor::gridKeys() {
  if (!ImGui::IsWindowFocused() || ImGui::IsAnyItemActive()) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.KeyCtrl || io.KeySuper) return;
  int rows = pattern().rows;
  auto move = [&](int delta) { row_ = std::clamp(row_ + delta, 0, rows - 1), scrollToCursor_ = true; };
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move(-1);
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move(1);
  if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) move(-16);
  if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) move(16);
  if (ImGui::IsKeyPressed(ImGuiKey_Home)) move(-rows);
  if (ImGui::IsKeyPressed(ImGuiKey_End)) move(rows);
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
    if (field_ > 0) field_--;
    else if (channel_ > 0) channel_--, field_ = 2;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
    if (field_ < 2) field_++;
    else if (channel_ + 1 < song_.channelCount()) channel_++, field_ = 0;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) channel_ = (channel_ + 1) % song_.channelCount();
  if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) musicAudio().playing() ? musicAudio().stop() : play(true);
  Cell& cell = pattern().cells[channel_][row_];
  if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
    if (field_ == 0) cell = Cell();
    else if (field_ == 1) cell.instrument = -1;
    else cell.effect = 0, cell.effectValue = 0;
    changed();
    move(step_);
    return;
  }
  if (field_ == 0) {
    if (ImGui::IsKeyPressed(ImGuiKey_1, false) || ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
      enterNote(kNoteOff);
      return;
    }
    for (auto& [key, offset] : kNoteKeys)
      if (ImGui::IsKeyPressed(key, false)) {
        int note = std::clamp((octave_ + 1) * 12 + offset, 12, 119);
        enterNote(note);
        preview(note);
        previewKey_ = key;
        return;
      }
    return;
  }
  static const ImGuiKey hexKeys[] = {ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7,
                                     ImGuiKey_8, ImGuiKey_9, ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E, ImGuiKey_F};
  if (field_ == 2) {
    // the letter picks the effect, a hex digit its value
    if (ImGui::IsKeyPressed(ImGuiKey_V, false) || ImGui::IsKeyPressed(ImGuiKey_R, false)) {
      cell.effect = ImGui::IsKeyPressed(ImGuiKey_V, false) ? kEffectVolume : kEffectRelease;
      changed();
      return;
    }
    for (int digit = 0; digit < 16; digit++)
      if (ImGui::IsKeyPressed(hexKeys[digit], false)) {
        if (!cell.effect) cell.effect = kEffectVolume;
        cell.effectValue = digit;
        changed();
        move(step_);
        return;
      }
    return;
  }
  {
    for (int digit = 0; digit < 16; digit++)
      if (ImGui::IsKeyPressed(hexKeys[digit], false)) {
        cell.instrument = ((std::max(0, cell.instrument) * 16) + digit) & 0xff;
        if (cell.instrument >= (int)song_.instruments.size()) cell.instrument = digit;
        instrument_ = std::clamp(cell.instrument, 0, (int)song_.instruments.size() - 1);
        changed();
        return;
      }
  }
}

void MusicEditor::patternGrid() {
  if (previewKey_ >= 0 && !ImGui::IsKeyDown((ImGuiKey)previewKey_)) previewKey_ = -1, stopPreview();
  gridKeys();
  Pattern& current = pattern();
  const int kChannels = song_.channelCount();  // this song's
  ImDrawList* draw = ImGui::GetWindowDrawList();
  float lineHeight = ImGui::GetTextLineHeightWithSpacing();
  float pad = em(8);
  float numberWidth = ImGui::CalcTextSize("000").x + pad;
  float noteWidth = ImGui::CalcTextSize("C#4 ").x, instrumentWidth = ImGui::CalcTextSize("00 ").x;
  float cellWidth = std::max(ImGui::CalcTextSize("C#4 00 V1").x + pad * 3, ImGui::CalcTextSize(F("Channel {0}", {"0"}).c_str()).x + pad * 2);
  auto fieldAt = [&](float x) { return x > noteWidth + instrumentWidth ? 2 : x > noteWidth ? 1 : 0; };
  bool playing = musicAudio().playing();
  SongPosition heard = musicAudio().position();
  // header
  ImVec2 origin = ImGui::GetCursorScreenPos();
  for (int channel = 0; channel < kChannels; channel++) {
    std::string name = F("Channel {0}", {std::to_string(channel)});
    draw->AddText(ImVec2(origin.x + numberWidth + channel * cellWidth + pad, origin.y), theme::col(theme::Role::UiTextDim), name.c_str());
  }
  ImGui::Dummy(ImVec2(numberWidth + kChannels * cellWidth, lineHeight));
  ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_None);
  draw = ImGui::GetWindowDrawList();
  ImGuiListClipper clipper;
  clipper.Begin(current.rows, lineHeight);
  if (scrollToCursor_) clipper.IncludeItemByIndex(row_);
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
      ImVec2 position = ImGui::GetCursorScreenPos();
      float width = numberWidth + kChannels * cellWidth;
      ImGui::InvisibleButton(("##row" + std::to_string(row)).c_str(), ImVec2(width, lineHeight));
      if (ImGui::IsItemClicked()) {
        float x = ImGui::GetMousePos().x - position.x - numberWidth;
        if (x >= 0) {
          channel_ = std::clamp((int)(x / cellWidth), 0, kChannels - 1);
          field_ = fieldAt(x - channel_ * cellWidth - pad);
        }
        row_ = row;
      }
      if (row % 4 == 0) draw->AddRectFilled(position, ImVec2(position.x + width, position.y + lineHeight), theme::col(theme::Role::UiTableRowAlt));
      if (playing && heard.order == order_ && heard.row == row)
        draw->AddRectFilled(position, ImVec2(position.x + width, position.y + lineHeight), theme::col(theme::Role::EdCurrentLine));
      if (row == row_) {
        draw->AddRect(position, ImVec2(position.x + width, position.y + lineHeight), theme::col(theme::Role::UiBorder));
        float cellX = position.x + numberWidth + channel_ * cellWidth + pad + (field_ == 0 ? 0 : field_ == 1 ? noteWidth : noteWidth + instrumentWidth);
        float fieldWidth = field_ ? ImGui::CalcTextSize("00").x : ImGui::CalcTextSize("C#4").x;
        draw->AddRectFilled(ImVec2(cellX - 2, position.y), ImVec2(cellX + fieldWidth + 2, position.y + lineHeight), theme::col(theme::Role::EdSelection));
      }
      char number[16];
      snprintf(number, sizeof number, "%02d", row);
      draw->AddText(position, theme::col(row % 4 == 0 ? theme::Role::UiText : theme::Role::UiTextDim), number);
      int lastInstrument = 0;
      for (int channel = 0; channel < kChannels; channel++) {
        // the instrument in force: the last one given in the pattern up to this row
        lastInstrument = 0;
        for (int earlier = 0; earlier <= row; earlier++)
          if (current.cells[channel][earlier].instrument >= 0) lastInstrument = current.cells[channel][earlier].instrument;
        const Cell& cell = current.cells[channel][row];
        float x = position.x + numberWidth + channel * cellWidth + pad;
        ImU32 noteColour = theme::col(theme::Role::UiTextDim);
        if (cell.note >= 0 && lastInstrument < (int)song_.instruments.size()) {
          const Instrument& instrument = song_.instruments[lastInstrument];
          noteColour = drum(instrument) ? theme::col(theme::Role::UiText) : tuningColour(song_.pitchOf(cell.note, instrument, clock_));
        } else if (cell.note == kNoteOff) {
          noteColour = theme::col(theme::Role::UiText);
        }
        draw->AddText(ImVec2(x, position.y), noteColour, cellNote(cell.note).c_str());
        char instrumentText[12] = "..";
        if (cell.instrument >= 0) snprintf(instrumentText, sizeof instrumentText, "%02X", cell.instrument & 0xff);
        bool missing = cell.instrument >= (int)song_.instruments.size();
        draw->AddText(ImVec2(x + noteWidth, position.y), theme::col(missing ? theme::Role::MsgError : cell.instrument >= 0 ? theme::Role::SynNumber : theme::Role::UiTextDim),
                      instrumentText);
        char effectText[4] = "..";
        if (cell.effect) snprintf(effectText, sizeof effectText, "%c%X", cell.effect, cell.effectValue & 15);
        draw->AddText(ImVec2(x + noteWidth + instrumentWidth, position.y), theme::col(cell.effect ? theme::Role::SynMnemonic : theme::Role::UiTextDim), effectText);
        float mouseInCell = ImGui::GetMousePos().x - position.x - numberWidth - channel * cellWidth;
        bool hoveredHere = ImGui::IsItemHovered() && mouseInCell >= 0 && mouseInCell < cellWidth;
        if (hoveredHere && fieldAt(mouseInCell - pad) == 2) {
          if (cell.effect == kEffectVolume) ImGui::SetTooltip("%s", F("V{0}: volume envelope {0} of the instrument", {std::to_string(cell.effectValue)}).c_str());
          else if (cell.effect == kEffectRelease) ImGui::SetTooltip("%s", F("R{0}: the note is released with release {0}", {std::to_string(cell.effectValue)}).c_str());
          else ImGui::SetTooltip("%s", T("Effect: V and a digit, the volume envelope; R and a digit, the release."));
        } else if (hoveredHere && cell.note >= 0 && lastInstrument < (int)song_.instruments.size() && !drum(song_.instruments[lastInstrument])) {
          {
            NotePitch pitch = song_.pitchOf(cell.note, song_.instruments[lastInstrument], clock_);
            if (song_.samples())
              ImGui::SetTooltip("%s", F("{0}: {1} Hz, exact", {noteName(cell.note + song_.tuning.transpose), std::to_string((int)std::lround(pitch.hz))}).c_str());
            else if (pitch.playable)
              ImGui::SetTooltip("%s", F("{0}: AUDC {1}, AUDF {2}, {3} Hz, {4} cents", {noteName(cell.note + song_.tuning.transpose), std::to_string(pitch.control),
                                                                                    std::to_string(pitch.frequency), std::to_string((int)std::lround(pitch.hz)),
                                                                                    (pitch.cents >= 0 ? "+" : "") + std::to_string((int)std::lround(pitch.cents))})
                                          .c_str());
            else
              ImGui::SetTooltip("%s", F("{0}: no allowed AUDC reaches it", {noteName(cell.note + song_.tuning.transpose)}).c_str());
          }
        }
      }
    }
  }
  if (scrollToCursor_) {
    float target = row_ * lineHeight;
    if (target < ImGui::GetScrollY() || target > ImGui::GetScrollY() + ImGui::GetWindowHeight() - lineHeight * 2)
      ImGui::SetScrollY(std::max(0.0f, target - ImGui::GetWindowHeight() / 2));
    scrollToCursor_ = false;
  }
  if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ImGui::SetWindowFocus();
  if (ImGui::IsWindowFocused()) gridKeys();
  ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// The register view: every frame of the pattern as the TIA gets it. The song is played
// from its start up to this order entry, so notes held from earlier patterns are right.

std::string MusicEditor::explain(const VoiceTrace& trace) const {
  if (!trace.active) return T("Silent: no note sounds.");
  std::string name = trace.instrument < (int)song_.instruments.size() ? song_.instruments[trace.instrument].name : "?";
  char number[8];
  snprintf(number, sizeof number, "%02X", trace.instrument & 0xff);
  std::string text = F("{0} with instrument {1} ({2})", {noteName(trace.note + song_.tuning.transpose), number, name}) + "\n";
  if (trace.released) text += F("Release {0}, frame {1}", {std::to_string(trace.releaseVariant), std::to_string(trace.frame)});
  else text += F("Volume envelope {0}, frame {1}", {std::to_string(trace.volumeVariant), std::to_string(trace.frame)});
  if (song_.samples()) return text;
  bool isDrum = trace.instrument < (int)song_.instruments.size() && song_.instruments[trace.instrument].drum;
  if (isDrum) return text + "\n" + T("Drum: its AUDC and AUDF lists.");
  if (!trace.pitch.playable) return text + "\n" + T("No allowed AUDC reaches this note: silent.");
  return text + "\n" + F("{0} cents from what is meant", {(trace.pitch.cents >= 0 ? "+" : "") + std::to_string((int)std::lround(trace.pitch.cents))});
}

void MusicEditor::registerView() {
  const int kChannels = song_.channelCount();
  bool samples = song_.samples();
  if (tracedRevision_ != revision_ || tracedOrder_ != order_ || tracedClock_ != clock_) {
    tracedRevision_ = revision_, tracedOrder_ = order_, tracedClock_ = clock_;
    traced_.clear();
    tracedBefore_ = FrameRegisters();
    Song once = song_;
    once.loopTo = -1;
    Player player;
    player.start(once, clock_);
    for (int frame = 0; frame < 200000 && player.playing(); frame++) {
      FrameRegisters registers = player.nextFrame();
      SongPosition position = player.position();
      if (!player.playing() || position.order > order_) break;
      if (position.order < order_) {
        tracedBefore_ = registers;
        continue;
      }
      TracedFrame entry{position, registers, {player.trace(0), player.trace(1), player.trace(2), player.trace(3)}};
      traced_.push_back(entry);
    }
  }
  // how many writes the player makes here: a register only needs writing when it changes
  int writes = 0;
  for (size_t index = 0; index < traced_.size(); index++) {
    const FrameRegisters& before = index ? traced_[index - 1].registers : tracedBefore_;
    for (int channel = 0; channel < kChannels; channel++)
      writes += samples ? 0 : (traced_[index].registers.control[channel] != before.control[channel]) + (traced_[index].registers.frequency[channel] != before.frequency[channel]) +
                (traced_[index].registers.volume[channel] != before.volume[channel]);
  }
  if (samples) {
    ImGui::TextDisabled("%s", F("{0} frames", {std::to_string(traced_.size())}).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("(V: the voice's volume, Hz: its pitch; the ARM mixes them into AUDV0 on every line)"));
  } else {
    ImGui::TextDisabled("%s", F("{0} frames, {1} register changes", {std::to_string(traced_.size()), std::to_string(writes)}).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T("(C, F, V: AUDC, AUDF, AUDV)"));
  }
  int fields = samples ? 2 : 3;
  bool playing = musicAudio().playing();
  SongPosition heard = musicAudio().position();
  ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
  if (!ImGui::BeginTable("##registers", 2 + fields * kChannels, flags)) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn(T("Frame"));
  ImGui::TableSetupColumn(T("Row"));
  // C0 F0 V0 C1 F1 V1: AUDC, AUDF and AUDV of each channel
  for (int channel = 0; channel < kChannels; channel++) {
    if (samples)
      for (const char* name : {"V", "Hz"}) ImGui::TableSetupColumn((name + std::to_string(channel)).c_str());
    else
      for (const char* name : {"C", "F", "V"}) ImGui::TableSetupColumn((name + std::to_string(channel)).c_str());
  }
  ImGui::TableHeadersRow();
  ImGuiListClipper clipper;
  clipper.Begin((int)traced_.size());
  while (clipper.Step()) {
    for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++) {
      const TracedFrame& frame = traced_[index];
      const FrameRegisters& before = index ? traced_[index - 1].registers : tracedBefore_;
      bool rowStart = index == 0 || traced_[index - 1].position.row != frame.position.row;
      ImGui::TableNextRow();
      if (playing && heard.order == order_ && heard.row == frame.position.row) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, theme::col(theme::Role::EdCurrentLine));
      else if (frame.position.row == row_) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, theme::col(theme::Role::EdSelection));
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%d", index);
      ImGui::TableNextColumn();
      if (rowStart) {
        char label[32];
        snprintf(label, sizeof label, "%02d##r%d", frame.position.row, index);
        if (ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) row_ = frame.position.row;
      }
      for (int channel = 0; channel < kChannels && samples; channel++) {
        double hz = frame.registers.sounding[channel] ? frame.registers.phaseStep[channel] * (clock_ / 2) / 4294967296.0 : 0;
        ImGui::TableNextColumn();
        ImGui::TextColored(theme::colv(frame.registers.sounding[channel] ? theme::Role::SynNumber : theme::Role::UiTextDim), "%2d", frame.registers.volume[channel]);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", explain(frame.voices[channel]).c_str());
        ImGui::TableNextColumn();
        if (hz > 0) ImGui::Text("%.1f", hz);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", explain(frame.voices[channel]).c_str());
      }
      for (int channel = 0; channel < kChannels && !samples; channel++) {
        int values[3] = {frame.registers.control[channel], frame.registers.frequency[channel], frame.registers.volume[channel]};
        int previous[3] = {before.control[channel], before.frequency[channel], before.volume[channel]};
        for (int field = 0; field < 3; field++) {
          ImGui::TableNextColumn();
          // what changes is written; what stays needs no write
          bool written = values[field] != previous[field];
          ImGui::TextColored(theme::colv(written ? theme::Role::SynNumber : theme::Role::UiTextDim), "%2d", values[field]);
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", explain(frame.voices[channel]).c_str());
        }
      }
    }
  }
  ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// The piano roll: rows across, notes up; each note lasts until the channel's next note or note off.

void MusicEditor::pianoRoll() {
  Pattern& current = pattern();
  const int kChannels = song_.channelCount();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  float laneHeight = em(11), rowWidth = em(18), labelWidth = em(40);
  int lowest = 24, highest = 96;  // C1 to C7
  int lanes = highest - lowest + 1;
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 size(labelWidth + current.rows * rowWidth, lanes * laneHeight);
  ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool hovered = ImGui::IsItemHovered();
  ImVec2 mouse = ImGui::GetMousePos();
  auto laneY = [&](int note) { return origin.y + (highest - note) * laneHeight; };
  auto rowX = [&](int row) { return origin.x + labelWidth + row * rowWidth; };
  for (int note = lowest; note <= highest; note++) {
    bool black = ((note % 12) == 1 || (note % 12) == 3 || (note % 12) == 6 || (note % 12) == 8 || (note % 12) == 10);
    float y = laneY(note);
    if (black) draw->AddRectFilled(ImVec2(rowX(0), y), ImVec2(rowX(current.rows), y + laneHeight), theme::col(theme::Role::UiTableRowAlt));
    if (note % 12 == 0) {
      draw->AddLine(ImVec2(rowX(0), y + laneHeight), ImVec2(rowX(current.rows), y + laneHeight), theme::col(theme::Role::UiBorder));
      draw->AddText(ImVec2(origin.x + em(4), y), theme::col(theme::Role::UiTextDim), noteName(note).c_str());
    }
  }
  for (int row = 0; row <= current.rows; row += 4)
    draw->AddLine(ImVec2(rowX(row), origin.y), ImVec2(rowX(row), origin.y + size.y), theme::col(theme::Role::UiBorder));
  draw->AddRectFilled(ImVec2(rowX(row_), origin.y), ImVec2(rowX(row_ + 1), origin.y + size.y), theme::col(theme::Role::EdSelection));
  SongPosition heard = musicAudio().position();
  if (musicAudio().playing() && heard.order == order_)
    draw->AddLine(ImVec2(rowX(heard.row), origin.y), ImVec2(rowX(heard.row), origin.y + size.y), theme::col(theme::Role::UiAccent), 2);
  // the notes: the current channel filled, the others outlined (drawn first)
  for (int pass = 0; pass < kChannels; pass++) {
    int channel = (channel_ + 1 + pass) % kChannels;
    int instrumentIndex = 0;
    for (int row = 0; row < current.rows; row++) {
      const Cell& cell = current.cells[channel][row];
      if (cell.instrument >= 0) instrumentIndex = cell.instrument;
      if (cell.note < lowest || cell.note > highest) continue;
      int end = row + 1;
      while (end < current.rows && current.cells[channel][end].note == kNoNote) end++;
      ImU32 colour = theme::col(theme::Role::UiText);
      if (instrumentIndex < (int)song_.instruments.size() && !drum(song_.instruments[instrumentIndex]))
        colour = tuningColour(song_.pitchOf(cell.note, song_.instruments[instrumentIndex], clock_));
      ImVec2 from(rowX(row) + 1, laneY(cell.note) + 1), to(rowX(end) - 1, laneY(cell.note) + laneHeight - 1);
      if (channel == channel_) draw->AddRectFilled(from, to, colour, em(2));
      else draw->AddRect(from, to, colour, em(2));
      if (hovered && mouse.x >= from.x && mouse.x < to.x && mouse.y >= from.y && mouse.y < to.y && instrumentIndex < (int)song_.instruments.size() &&
          !drum(song_.instruments[instrumentIndex])) {
        NotePitch pitch = song_.pitchOf(cell.note, song_.instruments[instrumentIndex], clock_);
        ImGui::SetTooltip("%s", F("Channel {0}, {1}: {2} cents", {std::to_string(channel), noteName(cell.note), (pitch.cents >= 0 ? "+" : "") + std::to_string((int)std::lround(pitch.cents))}).c_str());
      }
    }
  }
  // clicks: left places a note (or moves the cursor to one), right removes it
  if (hovered && mouse.x >= rowX(0)) {
    int row = std::clamp((int)((mouse.x - rowX(0)) / rowWidth), 0, current.rows - 1);
    int note = std::clamp(highest - (int)((mouse.y - origin.y) / laneHeight), lowest, highest);
    Cell& cell = current.cells[channel_][row];
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      row_ = row;
      if (cell.note != note) {
        cell.note = note, cell.instrument = instrument_, changed();
      }
      preview(note);
      previewMouse_ = true;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && cell.note != kNoNote) cell = Cell(), row_ = row, changed();
  }
  if (ImGui::IsWindowAppearing()) ImGui::SetScrollY(std::max(0.0f, laneY((octave_ + 2) * 12) - origin.y - ImGui::GetWindowHeight() / 2));
}

// ---------------------------------------------------------------------------
// The piano at the bottom: each key coloured by how in tune the instrument plays it.

void MusicEditor::keyboard() {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  ImVec2 origin = ImGui::GetCursorScreenPos();
  float width = ImGui::GetContentRegionAvail().x, height = em(64);
  int firstNote = std::clamp((octave_ - 1) * 12 + 12, 12, 72), octaves = 5;
  int whiteKeys = octaves * 7;
  float keyWidth = width / whiteKeys;
  ImGui::InvisibleButton("##piano", ImVec2(width, height));
  bool hovered = ImGui::IsItemHovered();
  const Instrument* instrument = instrument_ < (int)song_.instruments.size() ? &song_.instruments[instrument_] : nullptr;
  bool melodic = instrument && !drum(*instrument);
  static const int whiteSteps[] = {0, 2, 4, 5, 7, 9, 11};
  int hoveredNote = -1;
  auto keyColour = [&](int note, bool black) {
    ImU32 base = black ? IM_COL32(30, 30, 34, 255) : IM_COL32(236, 236, 230, 255);
    if (!melodic) return base;
    NotePitch pitch = song_.pitchOf(note, *instrument, clock_);
    ImU32 tint = tuningColour(pitch);
    float amount = pitch.playable ? 0.55f : 0.25f;
    auto channelMix = [&](int shift) { return (ImU32)((((base >> shift) & 255) * (1 - amount) + ((tint >> shift) & 255) * amount)) << shift; };
    return channelMix(0) | channelMix(8) | channelMix(16) | 0xff000000u;
  };
  ImVec2 mouse = ImGui::GetMousePos();
  // white keys, then the black ones over them (and checked first for the mouse)
  for (int key = 0; key < whiteKeys; key++) {
    int note = firstNote + (key / 7) * 12 + whiteSteps[key % 7];
    ImVec2 from(origin.x + key * keyWidth, origin.y), to(origin.x + (key + 1) * keyWidth - 1, origin.y + height);
    draw->AddRectFilled(from, to, keyColour(note, false), em(2));
    if (note % 12 == 0) draw->AddText(ImVec2(from.x + em(3), to.y - ImGui::GetTextLineHeight() - em(2)), IM_COL32(60, 60, 60, 255), noteName(note).c_str());
    if (hovered && mouse.x >= from.x && mouse.x < to.x) hoveredNote = note;
  }
  for (int key = 0; key < whiteKeys; key++) {
    int step = whiteSteps[key % 7];
    if (step == 4 || step == 11) continue;  // no black key after E and B
    int note = firstNote + (key / 7) * 12 + step + 1;
    float centre = origin.x + (key + 1) * keyWidth;
    ImVec2 from(centre - keyWidth * 0.32f, origin.y), to(centre + keyWidth * 0.32f, origin.y + height * 0.6f);
    draw->AddRectFilled(from, to, keyColour(note, true), em(2));
    if (hovered && mouse.x >= from.x && mouse.x < to.x && mouse.y < to.y) hoveredNote = note;
  }
  if (hoveredNote >= 0) {
    if (!melodic) ImGui::SetTooltip("%s", F("{0}: click to write it at the cursor", {noteName(hoveredNote)}).c_str());
    if (melodic) {
      NotePitch pitch = song_.pitchOf(hoveredNote, *instrument, clock_);
      if (song_.samples())
        ImGui::SetTooltip("%s\n%s", F("{0}: {1} Hz, exact", {noteName(hoveredNote), std::to_string((int)std::lround(pitch.hz))}).c_str(), T("Click: writes it at the cursor."));
      else if (pitch.playable)
        ImGui::SetTooltip("%s\n%s", F("{0}: AUDC {1}, AUDF {2}, {3} cents", {noteName(hoveredNote), std::to_string(pitch.control), std::to_string(pitch.frequency),
                                                                            (pitch.cents >= 0 ? "+" : "") + std::to_string((int)std::lround(pitch.cents))})
                                        .c_str(),
                          T("Click: writes it at the cursor."));
      else
        ImGui::SetTooltip("%s", F("{0}: no allowed AUDC reaches it", {noteName(hoveredNote)}).c_str());
    }
    // a click writes the note at the cursor, as typing it would, and plays it while held
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) enterNote(hoveredNote), preview(hoveredNote), previewMouse_ = true;
  }
}

// ---------------------------------------------------------------------------
// Instruments

// Bars to draw with the mouse; right click marks where the held part loops from.
bool MusicEditor::envelope(const char* id, std::vector<int>& values, int low, int high, int* loopStart, int minLength) {
  bool edited = false;
  ImGui::PushID(id);
  float barWidth = em(9), height = em(56);
  int count = std::max((int)values.size(), 1);
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 size(std::max(count * barWidth, em(60)), height);
  ImGui::InvisibleButton("##bars", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), theme::col(theme::Role::UiFrame));
  float zeroY = origin.y + height * (float)high / (float)(high - low);
  if (low < 0) draw->AddLine(ImVec2(origin.x, zeroY), ImVec2(origin.x + size.x, zeroY), theme::col(theme::Role::UiBorder));
  for (int index = 0; index < (int)values.size(); index++) {
    float valueY = origin.y + height * (float)(high - values[index]) / (float)(high - low);
    float baseY = low < 0 ? zeroY : origin.y + height;
    bool looped = loopStart && *loopStart >= 0 && index >= *loopStart;
    draw->AddRectFilled(ImVec2(origin.x + index * barWidth + 1, std::min(valueY, baseY)), ImVec2(origin.x + (index + 1) * barWidth - 1, std::max(valueY, baseY) + 1),
                        theme::col(looped ? theme::Role::UiAccentHover : theme::Role::UiAccent));
  }
  if (loopStart && *loopStart >= 0)
    draw->AddText(ImVec2(origin.x + *loopStart * barWidth, origin.y), theme::col(theme::Role::UiText), "↻");
  if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !values.empty()) {
    ImVec2 mouse = ImGui::GetMousePos();
    int index = std::clamp((int)((mouse.x - origin.x) / barWidth), 0, (int)values.size() - 1);
    int value = std::clamp((int)std::lround(high - (mouse.y - origin.y) / height * (high - low)), low, high);
    if (values[index] != value) values[index] = value, edited = true;
  }
  if (loopStart && ImGui::IsItemClicked(ImGuiMouseButton_Right) && !values.empty()) {
    int index = std::clamp((int)((ImGui::GetMousePos().x - origin.x) / barWidth), 0, (int)values.size() - 1);
    *loopStart = *loopStart == index ? -1 : index;
    edited = true;
  }
  if (ImGui::IsItemHovered() && !values.empty()) {
    int index = std::clamp((int)((ImGui::GetMousePos().x - origin.x) / barWidth), 0, (int)values.size() - 1);
    ImGui::SetTooltip("%s", F("Frame {0}: {1}", {std::to_string(index), std::to_string(values[index])}).c_str());
  }
  ImGui::SameLine();
  ImGui::BeginGroup();
  if (ImGui::SmallButton("+") && values.size() < 64) values.push_back(values.empty() ? 0 : values.back()), edited = true;
  if (ImGui::SmallButton("-") && (int)values.size() > minLength) {
    values.pop_back();
    if (loopStart && *loopStart >= (int)values.size()) *loopStart = -1;
    edited = true;
  }
  ImGui::EndGroup();
  ImGui::PopID();
  return edited;
}

// The variants of an envelope as small buttons: 0 1 2 ... plus one more and minus this one.
static bool variantButtons(const char* id, int count, int* current, bool* add, bool* remove) {
  ImGui::PushID(id);
  bool clicked = false;
  for (int index = 0; index < count; index++) {
    if (index) ImGui::SameLine(0, 2);
    bool selected = index == *current;
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, theme::col(theme::Role::UiAccent));
    if (ImGui::SmallButton(std::to_string(index).c_str())) *current = index, clicked = true;
    if (selected) ImGui::PopStyleColor();
  }
  ImGui::SameLine(0, em(8));
  *add = count < 16 && ImGui::SmallButton("+");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Another variant, a copy of this one."));
  *remove = false;
  if (count > 1) {
    ImGui::SameLine(0, 2);
    *remove = ImGui::SmallButton("-");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Removes this variant; the effects that used it fall back to the first one."));
  }
  ImGui::PopID();
  return clicked;
}

void MusicEditor::instrumentEditor() {
  if (song_.instruments.empty()) return;
  Instrument& instrument = song_.instruments[instrument_];
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputText("##iname", &instrument.name)) changed();
  if (song_.samples()) {
    waveEditor(instrument);
  } else {
    if (ImGui::RadioButton(T("Melodic"), !instrument.drum)) instrument.drum = false, changed();
    ImGui::SameLine();
    if (ImGui::RadioButton(T("Drum"), instrument.drum)) instrument.drum = true, changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Fixed AUDC and AUDF frame by frame, whatever the note."));
  }
  if (!drum(instrument) && !song_.samples()) {
    ImGui::SeparatorText(T("Sounds it may use"));
    ImGui::TextWrapped("%s", T("For each note the tuning picks the closest pitch among these."));
    for (auto& distortion : distortions()) {
      if (!distortion.tonal) continue;
      bool used = false;
      for (int control : distortion.controls) used = used || std::find(instrument.controls.begin(), instrument.controls.end(), control) != instrument.controls.end();
      std::string label = std::string(T(distortion.name)) + "  (AUDC " + std::to_string(distortion.controls[0]) + ")";
      if (ImGui::Checkbox(label.c_str(), &used)) {
        auto& list = instrument.controls;
        list.erase(std::remove_if(list.begin(), list.end(), [&](int control) { return std::find(distortion.controls.begin(), distortion.controls.end(), control) != distortion.controls.end(); }),
                   list.end());
        if (used) list.push_back(distortion.controls[0]);
        std::sort(list.begin(), list.end());
        changed();
      }
    }
  }
  bool add = false, remove = false;
  ImGui::SeparatorText(T("Volume"));
  volumeVariant_ = std::clamp(volumeVariant_, 0, (int)instrument.volumes.size() - 1);
  variantButtons("volumes", (int)instrument.volumes.size(), &volumeVariant_, &add, &remove);
  if (add) instrument.volumes.push_back(instrument.volumes[volumeVariant_]), volumeVariant_ = (int)instrument.volumes.size() - 1, changed();
  else if (remove) instrument.volumes.erase(instrument.volumes.begin() + volumeVariant_), volumeVariant_ = std::max(0, volumeVariant_ - 1), changed();
  ImGui::TextDisabled("%s", volumeVariant_ == 0 ? T("Frame by frame. Right click: the held note repeats from there.")
                                                : F("Used by notes with the effect V{0}. Right click: the held note repeats from there.", {std::to_string(volumeVariant_)}).c_str());
  Envelope& volume = instrument.volumes[volumeVariant_];
  if (envelope("volume", volume.values, 0, 15, &volume.loop, 1)) changed();
  ImGui::SeparatorText(T("Release"));
  releaseVariant_ = std::clamp(releaseVariant_, 0, (int)instrument.releases.size() - 1);
  variantButtons("releases", (int)instrument.releases.size(), &releaseVariant_, &add, &remove);
  if (add) instrument.releases.push_back(instrument.releases[releaseVariant_]), releaseVariant_ = (int)instrument.releases.size() - 1, changed();
  else if (remove) instrument.releases.erase(instrument.releases.begin() + releaseVariant_), releaseVariant_ = std::max(0, releaseVariant_ - 1), changed();
  ImGui::TextDisabled("%s", releaseVariant_ == 0 ? T("After a note off (===). Empty: silent at once.")
                                                 : F("After a note off when the note has the effect R{0}. Empty: silent at once.", {std::to_string(releaseVariant_)}).c_str());
  if (envelope("release", instrument.releases[releaseVariant_], 0, 15, nullptr, 0)) changed();
  if (song_.samples()) {
    ImGui::SeparatorText(T("Pitch"));
    ImGui::TextDisabled("%s", T("Added to the note frame by frame, in cents (positive is higher); the last value stays."));
    // a semitone for vibrato, one or two octaves for slides and drums
    int widest = 0;
    for (int value : instrument.pitch) widest = std::max(widest, std::abs(value));
    while (pitchRange_ < widest && pitchRange_ < 2400) pitchRange_ = pitchRange_ == 100 ? 1200 : 2400;
    static const int ranges[] = {100, 1200, 2400};
    for (int range : ranges) {
      ImGui::SameLine();
      if (ImGui::RadioButton(F("±{0}", {std::to_string(range)}).c_str(), pitchRange_ == range) && range >= widest) pitchRange_ = range;
    }
    if (envelope("pitch", instrument.pitch, -pitchRange_, pitchRange_, nullptr, 0)) changed();
  } else if (instrument.drum) {
    ImGui::SeparatorText("AUDC");
    if (envelope("audc", instrument.drumControl, 0, 15, nullptr, 1)) changed();
    ImGui::SeparatorText("AUDF");
    if (envelope("audf", instrument.drumFrequency, 0, 31, nullptr, 1)) changed();
  } else {
    ImGui::SeparatorText(T("Pitch"));
    bool cents = instrument.pitchUnit == PitchUnit::Cents;
    if (ImGui::RadioButton(T("in cents"), cents) && !cents) instrument.pitchUnit = PitchUnit::Cents, instrument.pitch.assign(instrument.pitch.size(), 0), changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The same interval on every note: each frame gets the AUDF that comes closest, note by note."));
    ImGui::SameLine();
    if (ImGui::RadioButton(T("in AUDF steps"), !cents) && cents) instrument.pitchUnit = PitchUnit::Audf, instrument.pitch.assign(instrument.pitch.size(), 0), changed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Added to AUDF as it is: one step is almost a semitone on high notes and a few cents on low ones."));
    ImGui::TextDisabled("%s", cents ? T("Added to the note frame by frame (positive is higher); the last value stays.")
                                    : T("Added to AUDF frame by frame (positive is lower); the last value stays."));
    if (cents ? envelope("pitch", instrument.pitch, -100, 100, nullptr, 0) : envelope("pitch", instrument.pitch, -8, 8, nullptr, 0)) changed();
    noteTuningEditor(instrument);
  }
  ImGui::Spacing();
  if (ImGui::Button(T("Try it"))) preview((octave_ + 1) * 12 + 9), previewMouse_ = true;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Plays an A of the current octave while the button is held."));
}

// The notes this instrument plays, each with what it writes frame by frame and how far
// off that is; any of them can be set by hand.
void MusicEditor::noteTuningEditor(Instrument& instrument) {
  ImGui::SeparatorText(T("Note by note"));
  ImGui::TextDisabled("%s", T("What each note of the song writes with this instrument. A note tuned by hand keeps its own AUDC and AUDF list."));
  std::vector<int> notes = song_.notesOf(instrument_);
  for (auto& [note, tuned] : instrument.byHand)
    if (std::find(notes.begin(), notes.end(), note) == notes.end()) notes.push_back(note);
  std::sort(notes.begin(), notes.end());
  if (notes.empty()) {
    ImGui::TextDisabled("%s", T("No note of the song uses it yet."));
    return;
  }
  auto centsText = [](double cents) { return (cents >= 0 ? "+" : "") + std::to_string((int)std::lround(cents)); };
  auto stepsColour = [&](const std::vector<PitchStep>& steps) {
    NotePitch worst;
    worst.playable = true;
    for (auto& step : steps) {
      if (!step.playable) worst.playable = false;
      if (std::fabs(step.cents) > std::fabs(worst.cents)) worst.cents = step.cents;
    }
    return tuningColour(worst);
  };
  if (ImGui::BeginTable("##notes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn(T("Note"));
    ImGui::TableSetupColumn("AUDC");
    ImGui::TableSetupColumn("AUDF");
    ImGui::TableSetupColumn(T("Cents"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int note : notes) {
      std::vector<PitchStep> steps = song_.pitchSequence(note - song_.tuning.transpose, instrument, clock_);
      bool byHand = instrument.byHand.count(note) > 0;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      std::string label = noteName(note) + (byHand ? " *" : "") + "##hand" + std::to_string(note);
      if (ImGui::Selectable(label.c_str(), handNote_ == note, ImGuiSelectableFlags_SpanAllColumns)) handNote_ = handNote_ == note ? -1 : note;
      if (byHand && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("* tuned by hand"));
      ImGui::TableNextColumn();
      if (steps[0].playable) ImGui::Text("%d", steps[0].control);
      ImGui::TableNextColumn();
      std::string frequencies;
      for (size_t index = 0; index < steps.size() && index < 8; index++) frequencies += (index ? " " : "") + std::to_string(steps[index].frequency);
      if (steps.size() > 8) frequencies += " ...";
      if (steps[0].playable) ImGui::TextUnformatted(frequencies.c_str());
      ImGui::TableNextColumn();
      std::string centsList;
      for (size_t index = 0; index < steps.size() && index < 8; index++) centsList += (index ? " " : "") + centsText(steps[index].cents);
      ImGui::PushStyleColor(ImGuiCol_Text, stepsColour(steps));
      ImGui::TextUnformatted(steps[0].playable ? centsList.c_str() : T("none"));
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
  if (handNote_ < 0 || std::find(notes.begin(), notes.end(), handNote_) == notes.end()) return;
  ImGui::PushID(handNote_);
  auto found = instrument.byHand.find(handNote_);
  ImGui::Text("%s", noteName(handNote_).c_str());
  ImGui::SameLine();
  if (ImGui::Button(T("Listen"))) preview(handNote_ - song_.tuning.transpose), previewMouse_ = true;
  ImGui::SameLine();
  if (found == instrument.byHand.end()) {
    if (ImGui::Button(T("Tune by hand"))) {
      std::vector<PitchStep> steps = song_.pitchSequence(handNote_ - song_.tuning.transpose, instrument, clock_);
      NoteTuning tuned;
      tuned.control = steps[0].playable ? steps[0].control : (instrument.controls.empty() ? 4 : instrument.controls[0]);
      for (auto& step : steps) tuned.frequencies.push_back(step.frequency);
      instrument.byHand[handNote_] = tuned;
      changed();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Starts from what it plays now; then each frame's AUDF, and the AUDC, are yours."));
    ImGui::PopID();
    return;
  }
  if (ImGui::Button(T("Back to automatic"))) {
    instrument.byHand.erase(found);
    changed();
    ImGui::PopID();
    return;
  }
  NoteTuning& tuned = found->second;
  ImGui::SetNextItemWidth(em(200));
  std::string current = distortionOf(tuned.control) ? std::string(T(distortionOf(tuned.control)->name)) + " (AUDC " + std::to_string(tuned.control) + ")" : "AUDC " + std::to_string(tuned.control);
  if (ImGui::BeginCombo("AUDC", current.c_str())) {
    for (auto& distortion : distortions()) {
      if (!distortion.tonal) continue;
      std::string label = std::string(T(distortion.name)) + " (AUDC " + std::to_string(distortion.controls[0]) + ")";
      if (ImGui::Selectable(label.c_str(), distortion.controls[0] == tuned.control)) tuned.control = distortion.controls[0], changed();
    }
    ImGui::EndCombo();
  }
  ImGui::TextDisabled("%s", T("AUDF frame by frame (higher is lower); the last one stays."));
  if (envelope("handaudf", tuned.frequencies, 0, 31, nullptr, 1)) changed();
  // how far off each frame is, under its bar
  std::vector<PitchStep> steps = song_.pitchSequence(handNote_ - song_.tuning.transpose, instrument, clock_);
  for (size_t index = 0; index < steps.size(); index++) {
    if (index) ImGui::SameLine(0, em(4));
    NotePitch shown;
    shown.playable = steps[index].playable, shown.cents = steps[index].cents;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(tuningColour(shown)), "%s", centsText(steps[index].cents).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", F("Frame {0}: AUDF {1}, {2} cents from the note", {std::to_string(index), std::to_string(steps[index].frequency), centsText(steps[index].cents)}).c_str());
  }
  ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Export

void MusicEditor::exportPopup() {
  if (!ImGui::BeginPopup("##export")) return;
  ImGui::TextUnformatted(T("Export the song for the console"));
  ImGui::Separator();
  if (song_.samples()) {
    static std::string chosen;  // the target picked while the popup is open
    if (ImGui::IsWindowAppearing()) chosen = song_.exportTo == "dpc" || song_.exportTo == "cdfj" ? song_.exportTo : "c";
    if (ImGui::RadioButton(T("C for an ELF cartridge"), chosen == "c")) chosen = "c";
    if (ImGui::RadioButton(T("C for a CDFJ cartridge"), chosen == "cdfj")) chosen = "cdfj";
    if (ImGui::RadioButton(T("DASM for a DPC+ cartridge"), chosen == "dpc")) chosen = "dpc";
    ImGui::Spacing();
    if (chosen == "c") {
      ImGui::TextDisabled("%s", T("Calls: music_init() once; music_frame() once per frame, in the overblank; music_effect(n);"));
      ImGui::TextDisabled("%s", T("then music_level[line] to AUDV0 on every line of the kernel, and music_nibbles to $E0-$FE for the 63 lines of the overblank."));
    } else if (chosen == "cdfj") {
      ImGui::TextDisabled("%s", T("<name>.music.h for the ARM code. Define MUSIC_CDFJ_WAVE(voice, samples) and MUSIC_CDFJ_NOTE(voice, frequency, restart) first:"));
      ImGui::TextDisabled("%s", T("copy the samples to the voice's wave in display RAM, and give the frequency to setNote. The kernel copies the amplitude to AUDV0 on every line."));
    } else {
      ImGui::TextDisabled("%s", T("<name>.music.asm, included three times: MUSIC_PART 1 with the code, 2 in the display data, 3 as the frequency table."));
      ImGui::TextDisabled("%s", T("Calls: MusicInit once, MusicFrame once per frame; the kernel does lda AMPLITUDE / sta AUDV0 on every line, with AUDC0 = 0."));
    }
    if (chosen != "c") {
      ImGui::TextDisabled("%s", T("Up to 3 voices; each voice's volume has 15 / voices steps, and noise repeats every 32 steps."));
      ImGui::TextDisabled("%s", T("The cartridge's music is DPC+'s, by Darrell Spice Jr. and Fred Quimby (first in Stay Frosty 2); CDF and CDFJ reuse it."));
      if (song_.voices > 3) ImGui::TextColored(theme::colv(theme::Role::MsgWarning), "%s", T("This song has more than 3 voices."));
    }
    ImGui::Spacing();
    if (theme::primaryButton(T("Export"))) {
      if (song_.exportTo != chosen) song_.exportTo = chosen, changed();
      exportSongFile(false);
      ImGui::CloseCurrentPopup();
    }
    if (!song_.exportTo.empty()) {
      ImGui::SameLine();
      if (ImGui::Button(T("Stop exporting"))) song_.exportTo.clear(), changed(), ImGui::CloseCurrentPopup();
    }
    if (!exportProblems_.empty()) {
      ImGui::SeparatorText(T("Last export"));
      for (auto& problem : exportProblems_) ImGui::BulletText("%s", problem.c_str());
    }
    ImGui::EndPopup();
    return;
  }
  bool basic = song_.exportTo == "bb";
  if (ImGui::RadioButton(T("DASM (assembly)"), !basic)) basic = false;
  if (ImGui::RadioButton("batari Basic", basic)) basic = true;
  if (basic) {
    ImGui::SetNextItemWidth(em(60));
    if (ImGui::InputText(T("first variable"), &song_.exportRam)) changed();
    ImGui::TextDisabled("%s", F("The player takes {0} variables from there: h takes h to z.", {std::to_string(kMusicRamBytes)}).c_str());
  } else {
    ImGui::TextDisabled("%s", F("Define MUSIC_RAM, {0} bytes of zero page, before the include.", {std::to_string(kMusicRamBytes)}).c_str());
  }
  ImGui::TextDisabled("%s", T("Calls: MusicInit once, MusicFrame once per frame, MusicEffect with an effect in A."));
  ImGui::Spacing();
  if (theme::primaryButton(T("Export"))) {
    std::string target = basic ? "bb" : "dasm";
    if (song_.exportTo != target) song_.exportTo = target, changed();
    exportSongFile(false);
    ImGui::CloseCurrentPopup();
  }
  if (!song_.exportTo.empty()) {
    ImGui::SameLine();
    if (ImGui::Button(T("Stop exporting"))) song_.exportTo.clear(), changed(), ImGui::CloseCurrentPopup();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Saving no longer writes the .music.asm (the file stays)."));
  }
  if (!exportProblems_.empty()) {
    ImGui::SeparatorText(T("Last export"));
    for (auto& problem : exportProblems_) ImGui::BulletText("%s", problem.c_str());
  }
  ImGui::EndPopup();
}

// Sound effects: an instrument and a note the game plays when something happens.
void MusicEditor::effectsPanel() {
  ImGui::TextWrapped("%s", T("Sounds the game plays when something happens. While one sounds, the song's notes on its channel wait."));
  ImGui::BeginChild("##effects", ImVec2(0, em(110)), ImGuiChildFlags_Borders);
  for (int index = 0; index < (int)song_.effects.size(); index++) {
    std::string label = song_.effects[index].name + "##e" + std::to_string(index);
    if (ImGui::Selectable(label.c_str(), index == effect_)) effect_ = index;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", effectSymbol(song_.effects[index].name).c_str());
  }
  ImGui::EndChild();
  ImGui::PushID("effectButtons");
  if (ImGui::SmallButton(T("New"))) {
    SoundEffect effect;
    effect.name = F("effect {0}", {std::to_string(song_.effects.size() + 1)});
    effect.instrument = instrument_;
    song_.effects.push_back(effect);
    effect_ = (int)song_.effects.size() - 1, changed();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(T("Remove")) && effect_ < (int)song_.effects.size()) {
    song_.effects.erase(song_.effects.begin() + effect_);
    effect_ = std::max(0, effect_ - 1), changed();
  }
  ImGui::PopID();
  if (effect_ >= (int)song_.effects.size()) return;
  SoundEffect& effect = song_.effects[effect_];
  ImGui::Spacing();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputText("##ename", &effect.name)) changed();
  std::string current = effect.instrument < (int)song_.instruments.size() ? song_.instruments[effect.instrument].name : "?";
  ImGui::SetNextItemWidth(em(160));
  if (ImGui::BeginCombo(T("Instrument"), current.c_str())) {
    for (int index = 0; index < (int)song_.instruments.size(); index++)
      if (ImGui::Selectable((song_.instruments[index].name + "##ei" + std::to_string(index)).c_str(), index == effect.instrument)) effect.instrument = index, changed();
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(em(160));
  int note = effect.note;
  if (ImGui::SliderInt(T("Note"), &note, 24, 108, noteName(note).c_str())) effect.note = note, changed();
  int channel = effect.channel;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(T("Channel"));
  ImGui::SameLine();
  for (int index = 0; index < song_.channelCount(); index++) {
    ImGui::SameLine();
    if (ImGui::RadioButton(std::to_string(index).c_str(), channel == index)) effect.channel = index, changed();
  }
  if (ImGui::Button(T("Play it"))) {
    std::string error;
    if (!musicAudio().ready(&error)) message_ = F("No sound output: {0}", {error});
    else musicAudio().playEffect(song_, tv_, effect_);
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Also while the song plays, to hear how the two share the channel."));
  if (song_.samples()) ImGui::TextDisabled("%s", F("In the game: music_effect({0});", {effectSymbol(effect.name)}).c_str());
  else ImGui::TextDisabled("%s", F("In the game: lda #{0} / jsr MusicEffect", {effectSymbol(effect.name)}).c_str());
}

// ---------------------------------------------------------------------------
// Tuning: how far off each note of the song is, and the search for a better tuning.

void MusicEditor::tuningPanel() {
  if (song_.samples()) {
    ImGui::TextWrapped("%s", T("Played as samples, every note is exact: the ARM keeps a 32-bit phase for each voice (direct digital synthesis), so nothing needs tuning."));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::col(theme::Role::UiTextDim));
    ImGui::TextWrapped("%s", T("Playing 4-bit samples by writing AUDV with AUDC = 0 is an old 2600 technique; who did it first is still to be found. The mix goes through Chris Brenner's formula of the TIA's output, in Thomas Jentzsch's form. On the overblank's lines the samples are played by the ELF loader's own overblank program (United Carts of Atari firmware, Stella, Gopher2600), whose write to $2D becomes a write to AUDV0."));
    ImGui::PopStyleColor();
    ImGui::SetNextItemWidth(em(110));
    int transpose = song_.tuning.transpose;
    if (ImGui::InputInt(T("Transpose"), &transpose)) song_.tuning.transpose = std::clamp(transpose, -24, 24), changed();
    ImGui::SetNextItemWidth(em(110));
    int reference = song_.tuning.referenceCents;
    if (ImGui::SliderInt(T("A4 (cents)"), &reference, -50, 50, "%+d")) song_.tuning.referenceCents = reference, changed();
    return;
  }
  ImGui::TextWrapped("%s", F("{0}: the TIA's audio clock is {1} Hz.", {tv_, std::to_string((int)std::lround(clock_))}).c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, theme::col(theme::Role::UiTextDim));
  ImGui::TextWrapped("%s", T("What you hear is the TIA counter by counter, as Chris Brenner described it (\"TIA Sounding Off in the Digital Domain\"), with the two channels mixed by his formula as Thomas Jentzsch simplified it. The pitches are those of Ron Fries' TIASOUND and of the tables by Eckhard Stolberg and Paul Slocum."));
  ImGui::PopStyleColor();
  ImGui::SetNextItemWidth(em(110));
  int transpose = song_.tuning.transpose;
  if (ImGui::InputInt(T("Transpose"), &transpose)) song_.tuning.transpose = std::clamp(transpose, -24, 24), changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Semitones added to every note when it is played; the notes stay as written."));
  ImGui::SetNextItemWidth(em(110));
  int reference = song_.tuning.referenceCents;
  if (ImGui::SliderInt(T("A4 (cents)"), &reference, -50, 50, "%+d")) song_.tuning.referenceCents = reference, changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Moves the whole tuning: the song is measured against A4 = 440 Hz plus these cents."));
  int goal = song_.tuning.goal == TuningGoal::Average ? 1 : 0;
  ImGui::TextUnformatted(T("Tune for"));
  ImGui::SameLine();
  if (ImGui::RadioButton(T("the worst note"), goal == 0)) song_.tuning.goal = TuningGoal::WorstNote, changed();
  ImGui::SameLine();
  if (ImGui::RadioButton(T("the average"), goal == 1)) song_.tuning.goal = TuningGoal::Average, changed();
  auto notes = song_.notes();
  SongTuning now = measureSong(notes, clock_, song_.tuning.transpose, song_.tuning.referenceCents);
  if (ImGui::Button(T("Tune the song")) && !notes.empty()) {
    SongTuning best = tuneSong(notes, clock_, song_.tuning.goal);
    song_.tuning.transpose = best.transpose, song_.tuning.referenceCents = best.referenceCents;
    message_ = F("Tuned: {0} semitones, A4 {1} cents; worst note {2} → {3} cents, average {4} → {5}",
                 {std::to_string(best.transpose), (best.referenceCents >= 0 ? "+" : "") + std::to_string(best.referenceCents),
                  std::to_string((int)std::lround(now.worstCents)), std::to_string((int)std::lround(best.worstCents)),
                  std::to_string((int)std::lround(now.averageCents)), std::to_string((int)std::lround(best.averageCents))});
    changed();
    now = best;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T("Tries every transposition from -6 to +6 semitones and every A4 from -50 to +50 cents, with the sounds each instrument may use. Tuning against the song rather than A440 is how Paul Slocum's guide (2003) tuned the TIA; TIA Music Explorer (pizzatari) explores the same two settings."));
  ImGui::Separator();
  if (notes.empty()) {
    ImGui::TextDisabled("%s", T("The song has no melodic notes yet."));
    return;
  }
  ImGui::Text("%s", F("Worst note {0} cents, average {1}", {std::to_string((int)std::lround(now.worstCents)), std::to_string((int)std::lround(now.averageCents))}).c_str());
  if (now.unplayable > 0) ImGui::TextColored(theme::colv(theme::Role::MsgError), "%s", F("{0} notes cannot be played", {std::to_string(now.unplayable)}).c_str());
  // each different note and sound set once, with how often it is played
  std::map<std::pair<int, std::vector<int>>, std::pair<NotePitch, int>> rows;
  for (size_t index = 0; index < notes.size(); index++) {
    auto key = std::make_pair(notes[index].note, notes[index].allowedControls);
    auto found = rows.find(key);
    if (found == rows.end()) rows.emplace(key, std::make_pair(now.pitches[index], 1));
    else found->second.second++;
  }
  if (ImGui::BeginTable("##tuning", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn(T("Note"));
    ImGui::TableSetupColumn("AUDC");
    ImGui::TableSetupColumn("AUDF");
    ImGui::TableSetupColumn(T("Cents"));
    ImGui::TableSetupColumn("×");
    ImGui::TableHeadersRow();
    for (auto& [key, entry] : rows) {
      const NotePitch& pitch = entry.first;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(noteName(key.first).c_str());
      if (song_.tuning.transpose != 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", F("Heard as {0}", {noteName(key.first + song_.tuning.transpose)}).c_str());
      ImGui::TableNextColumn();
      if (pitch.playable) ImGui::Text("%d", pitch.control);
      ImGui::TableNextColumn();
      if (pitch.playable) ImGui::Text("%d", pitch.frequency);
      ImGui::TableNextColumn();
      ImGui::PushStyleColor(ImGuiCol_Text, tuningColour(pitch));
      if (pitch.playable) ImGui::Text("%+d", (int)std::lround(pitch.cents));
      else ImGui::TextUnformatted(T("none"));
      ImGui::PopStyleColor();
      ImGui::TableNextColumn();
      ImGui::Text("%d", entry.second);
    }
    ImGui::EndTable();
  }
}

// ---------------------------------------------------------------------------
// What plays the song: the TIA's two channels, or voices mixed into samples (ELF).

void MusicEditor::soundPanel() {
  ImGui::SeparatorText(T("Sound"));
  bool samples = song_.samples();
  if (ImGui::RadioButton(T("TIA"), !samples) && samples) song_.target = "tia", channel_ = std::min(channel_, kChannels - 1), changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The TIA's two channels: AUDC, AUDF and AUDV once per frame. Any cartridge."));
  ImGui::SameLine();
  if (ImGui::RadioButton(T("Samples (ELF)"), samples) && !samples) {
    song_.target = "samples";
    if (song_.exportTo == "dasm" || song_.exportTo == "bb") song_.exportTo = "c";
    changed();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T("Voices with waveforms, played as one 4-bit sample per scanline in AUDV0, with exact pitch: mixed by the ARM of an ELF cartridge (C), or played by a DPC+ or CDFJ cartridge (up to 3 voices)."));
  if (!song_.samples()) {
    if (song_.exportTo == "c" || song_.exportTo == "dpc" || song_.exportTo == "cdfj") song_.exportTo.clear();
    return;
  }
  ImGui::SetNextItemWidth(em(80));
  int voices = song_.voices;
  if (ImGui::InputInt(T("Voices"), &voices)) song_.voices = std::clamp(voices, 1, kMaxChannels), channel_ = std::min(channel_, song_.channelCount() - 1), changed();
  ImGui::SetNextItemWidth(em(80));
  int gain = song_.gain;
  if (ImGui::InputInt(T("Loudness"), &gain, 1, 4)) song_.gain = std::clamp(gain, 4, 64), changed();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T("16: every voice at full volume together reaches AUDV 15. More is louder, and a mix that goes past 15 is clipped."));
}

void MusicEditor::waveEditor(Instrument& instrument) {
  ImGui::SeparatorText(T("Waveform"));
  static const char* names[] = {N_("Square"), N_("Triangle"), N_("Sawtooth"), N_("Sine"), N_("Noise"), N_("Drawn")};
  ImGui::SetNextItemWidth(em(140));
  if (ImGui::BeginCombo("##wave", T(names[(int)instrument.wave]))) {
    for (int index = 0; index < 6; index++)
      if (ImGui::Selectable(T(names[index]), index == (int)instrument.wave)) {
        if ((Waveform)index == Waveform::Drawn && instrument.wave != Waveform::Drawn) {  // starts from what it had
          auto levels = waveLevels(instrument);
          instrument.drawn.assign(levels.begin(), levels.end());
        }
        instrument.wave = (Waveform)index, changed();
      }
    ImGui::EndCombo();
  }
  if (instrument.wave == Waveform::Square) {
    ImGui::SetNextItemWidth(em(140));
    if (ImGui::SliderInt(T("Duty (of 32)"), &instrument.duty, 1, 31)) changed();
  } else if (instrument.wave == Waveform::Noise) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Register"));
    for (int bits : {4, 5, 9, 15}) {
      ImGui::SameLine();
      if (ImGui::RadioButton(F("{0} bits", {std::to_string(bits)}).c_str(), instrument.noiseBits == bits)) instrument.noiseBits = bits, changed();
    }
    ImGui::TextDisabled("%s", T("The register moves 32 times per cycle of the note: the note sets how bright the noise is. 4, 5 and 9 bits repeat like the TIA's."));
  } else if (instrument.wave == Waveform::Drawn) {
    instrument.drawn.resize(32, 0);
    ImGui::TextDisabled("%s", T("One cycle, 32 steps of 0 to 15."));
    if (envelope("drawn", instrument.drawn, 0, 15, nullptr, 32)) changed();
  }
  if (instrument.wave != Waveform::Drawn && instrument.wave != Waveform::Noise) {
    // the cycle as it is played
    auto levels = waveLevels(instrument);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float step = em(4), height = em(30);
    ImGui::Dummy(ImVec2(step * 32, height));
    draw->AddRectFilled(origin, ImVec2(origin.x + step * 32, origin.y + height), theme::col(theme::Role::UiFrame));
    for (int index = 0; index < 32; index++)
      draw->AddRectFilled(ImVec2(origin.x + index * step, origin.y + height * (15 - levels[index]) / 15.0f), ImVec2(origin.x + (index + 1) * step - 1, origin.y + height),
                          theme::col(theme::Role::UiAccent));
  }
}

}  // namespace vcs::music
