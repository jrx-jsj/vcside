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
  return true;
}

void MusicEditor::undo() {
  if (undo_.empty()) return;
  redo_.push_back(song_);
  song_ = undo_.back();
  undo_.pop_back();
  committed_ = song_;
  dirty_ = true;
  musicAudio().updateSong(song_);
}

void MusicEditor::redo() {
  if (redo_.empty()) return;
  undo_.push_back(song_);
  song_ = redo_.back();
  redo_.pop_back();
  committed_ = song_;
  dirty_ = true;
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
  orderPanel();
  ImGui::Spacing();
  instrumentPanel();
  ImGui::EndChild();
  ImGui::SameLine();
  float centre = std::max(em(200), ImGui::GetContentRegionAvail().x - right - ImGui::GetStyle().ItemSpacing.x);
  ImGui::BeginChild("##centre", ImVec2(centre, height), ImGuiChildFlags_None);
  float keysHeight = em(70);
  ImGui::BeginChild(roll_ ? "##roll" : "##grid", ImVec2(0, ImGui::GetContentRegionAvail().y - keysHeight - ImGui::GetStyle().ItemSpacing.y),
                    ImGuiChildFlags_Borders, roll_ ? ImGuiWindowFlags_HorizontalScrollbar : 0);
  if (roll_) pianoRoll();
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
  for (int channel = 0; channel < kChannels; channel++) {
    ImGui::SameLine();
    bool heard = !muted_[channel];
    std::string label = F("Channel {0}", {std::to_string(channel)});
    if (ImGui::Checkbox(label.c_str(), &heard)) muted_[channel] = !heard, musicAudio().setMuted(channel, muted_[channel]);
  }
  ImGui::SameLine(0, em(20));
  if (ImGui::RadioButton(T("Tracker"), !roll_)) roll_ = false;
  ImGui::SameLine();
  if (ImGui::RadioButton(T("Piano roll"), roll_)) roll_ = true;
  ImGui::SameLine(0, em(20));
  if (ImGui::Button(T("Save as WAV"))) {
    std::string wavPath = fs::path(path_).replace_extension(".wav").string(), error;
    auto samples = renderSong(song_, tv_, 48000, 60 * 60 * 10);
    message_ = writeWav(wavPath, samples, 48000, &error) ? F("Written {0} ({1} s)", {fs::path(wavPath).filename().string(), std::to_string(samples.size() / 48000)}) : error;
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The song once through, next to this file, as the TIA plays it."));
  // second line: timing and typing
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
}

void MusicEditor::instrumentPanel() {
  ImGui::SeparatorText(T("Instruments"));
  ImGui::BeginChild("##instruments", ImVec2(0, em(140)), ImGuiChildFlags_Borders);
  for (int index = 0; index < (int)song_.instruments.size(); index++) {
    char label[96];
    snprintf(label, sizeof label, "%02X  %s##i%d", index, song_.instruments[index].name.c_str(), index);
    if (ImGui::Selectable(label, index == instrument_)) instrument_ = index;
    if (song_.instruments[index].drum) {
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
    else if (channel_ > 0) channel_--, field_ = 1;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
    if (field_ < 1) field_++;
    else if (channel_ + 1 < kChannels) channel_++, field_ = 0;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) channel_ = (channel_ + 1) % kChannels;
  if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) musicAudio().playing() ? musicAudio().stop() : play(true);
  Cell& cell = pattern().cells[channel_][row_];
  if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
    if (field_ == 0) cell = Cell();
    else cell.instrument = -1;
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
  } else {
    static const ImGuiKey hexKeys[] = {ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7,
                                       ImGuiKey_8, ImGuiKey_9, ImGuiKey_A, ImGuiKey_B, ImGuiKey_C, ImGuiKey_D, ImGuiKey_E, ImGuiKey_F};
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
  ImDrawList* draw = ImGui::GetWindowDrawList();
  float lineHeight = ImGui::GetTextLineHeightWithSpacing();
  float pad = em(8);
  float numberWidth = ImGui::CalcTextSize("000").x + pad;
  float noteWidth = ImGui::CalcTextSize("C#4 ").x, cellWidth = std::max(ImGui::CalcTextSize("C#4 00").x + pad * 3, ImGui::CalcTextSize(F("Channel {0}", {"0"}).c_str()).x + pad * 2);
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
          field_ = x - channel_ * cellWidth > pad + noteWidth ? 1 : 0;
        }
        row_ = row;
      }
      if (row % 4 == 0) draw->AddRectFilled(position, ImVec2(position.x + width, position.y + lineHeight), theme::col(theme::Role::UiTableRowAlt));
      if (playing && heard.order == order_ && heard.row == row)
        draw->AddRectFilled(position, ImVec2(position.x + width, position.y + lineHeight), theme::col(theme::Role::EdCurrentLine));
      if (row == row_) {
        draw->AddRect(position, ImVec2(position.x + width, position.y + lineHeight), theme::col(theme::Role::UiBorder));
        float cellX = position.x + numberWidth + channel_ * cellWidth + pad + (field_ ? noteWidth : 0);
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
          noteColour = instrument.drum ? theme::col(theme::Role::UiText) : tuningColour(song_.pitchOf(cell.note, instrument, clock_));
        } else if (cell.note == kNoteOff) {
          noteColour = theme::col(theme::Role::UiText);
        }
        draw->AddText(ImVec2(x, position.y), noteColour, cellNote(cell.note).c_str());
        char instrumentText[12] = "..";
        if (cell.instrument >= 0) snprintf(instrumentText, sizeof instrumentText, "%02X", cell.instrument & 0xff);
        bool missing = cell.instrument >= (int)song_.instruments.size();
        draw->AddText(ImVec2(x + noteWidth, position.y), theme::col(missing ? theme::Role::MsgError : cell.instrument >= 0 ? theme::Role::SynNumber : theme::Role::UiTextDim),
                      instrumentText);
        if (ImGui::IsItemHovered() && cell.note >= 0 && lastInstrument < (int)song_.instruments.size() && !song_.instruments[lastInstrument].drum) {
          float mouseX = ImGui::GetMousePos().x - position.x - numberWidth;
          if ((int)(mouseX / cellWidth) == channel) {
            NotePitch pitch = song_.pitchOf(cell.note, song_.instruments[lastInstrument], clock_);
            if (pitch.playable)
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
// The piano roll: rows across, notes up; each note lasts until the channel's next note or note off.

void MusicEditor::pianoRoll() {
  Pattern& current = pattern();
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
  // the notes: the current channel filled, the other one outlined
  for (int pass = 0; pass < kChannels; pass++) {
    int channel = pass == 0 ? (channel_ + 1) % kChannels : channel_;
    int instrumentIndex = 0;
    for (int row = 0; row < current.rows; row++) {
      const Cell& cell = current.cells[channel][row];
      if (cell.instrument >= 0) instrumentIndex = cell.instrument;
      if (cell.note < lowest || cell.note > highest) continue;
      int end = row + 1;
      while (end < current.rows && current.cells[channel][end].note == kNoNote) end++;
      ImU32 colour = theme::col(theme::Role::UiText);
      if (instrumentIndex < (int)song_.instruments.size() && !song_.instruments[instrumentIndex].drum)
        colour = tuningColour(song_.pitchOf(cell.note, song_.instruments[instrumentIndex], clock_));
      ImVec2 from(rowX(row) + 1, laneY(cell.note) + 1), to(rowX(end) - 1, laneY(cell.note) + laneHeight - 1);
      if (channel == channel_) draw->AddRectFilled(from, to, colour, em(2));
      else draw->AddRect(from, to, colour, em(2));
      if (hovered && mouse.x >= from.x && mouse.x < to.x && mouse.y >= from.y && mouse.y < to.y && instrumentIndex < (int)song_.instruments.size() &&
          !song_.instruments[instrumentIndex].drum) {
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
  bool melodic = instrument && !instrument->drum;
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
      if (pitch.playable)
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

void MusicEditor::instrumentEditor() {
  if (song_.instruments.empty()) return;
  Instrument& instrument = song_.instruments[instrument_];
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputText("##iname", &instrument.name)) changed();
  if (ImGui::RadioButton(T("Melodic"), !instrument.drum)) instrument.drum = false, changed();
  ImGui::SameLine();
  if (ImGui::RadioButton(T("Drum"), instrument.drum)) instrument.drum = true, changed();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Fixed AUDC and AUDF frame by frame, whatever the note."));
  if (!instrument.drum) {
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
  ImGui::SeparatorText(T("Volume"));
  ImGui::TextDisabled("%s", T("Frame by frame. Right click: the held note repeats from there."));
  if (envelope("volume", instrument.volume, 0, 15, &instrument.loopStart, 1)) changed();
  ImGui::SeparatorText(T("Release"));
  ImGui::TextDisabled("%s", T("After a note off (===). Empty: silent at once."));
  if (envelope("release", instrument.release, 0, 15, nullptr, 0)) changed();
  if (instrument.drum) {
    ImGui::SeparatorText("AUDC");
    if (envelope("audc", instrument.drumControl, 0, 15, nullptr, 1)) changed();
    ImGui::SeparatorText("AUDF");
    if (envelope("audf", instrument.drumFrequency, 0, 31, nullptr, 1)) changed();
  } else {
    ImGui::SeparatorText(T("Pitch"));
    ImGui::TextDisabled("%s", T("Added to AUDF frame by frame (positive is lower); the last value stays."));
    if (envelope("pitch", instrument.pitch, -8, 8, nullptr, 0)) changed();
  }
  ImGui::Spacing();
  if (ImGui::Button(T("Try it"))) preview((octave_ + 1) * 12 + 9), previewMouse_ = true;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Plays an A of the current octave while the button is held."));
}

// ---------------------------------------------------------------------------
// Tuning: how far off each note of the song is, and the search for a better tuning.

void MusicEditor::tuningPanel() {
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

}  // namespace vcs::music
