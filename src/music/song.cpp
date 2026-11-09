// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "song.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "../i18n.h"

using json = nlohmann::json;

namespace vcs::music {

void Pattern::resize(int rowCount) {
  rows = std::clamp(rowCount, 1, 256);
  for (auto& channel : cells) channel.resize(rows);
}

std::vector<SongNote> Song::notes() const {
  std::vector<SongNote> list;
  for (int orderIndex : order) {
    if (orderIndex < 0 || orderIndex >= (int)patterns.size()) continue;
    const Pattern& pattern = patterns[orderIndex];
    for (int channel = 0; channel < kChannels; channel++) {
      int instrumentIndex = 0;
      for (const Cell& cell : pattern.cells[channel]) {
        if (cell.instrument >= 0) instrumentIndex = cell.instrument;
        if (cell.note < 0 || instrumentIndex >= (int)instruments.size() || instruments[instrumentIndex].drum) continue;
        list.push_back({cell.note, instruments[instrumentIndex].controls});
      }
    }
  }
  return list;
}

NotePitch Song::pitchOf(int note, const Instrument& instrument, double audioClock) const {
  NotePitch pitch = closestPitch(note + tuning.transpose, audioClock, instrument.controls, tuning.referenceCents);
  pitch.note = note;
  return pitch;
}

Song blankSong() {
  Song song;
  Instrument lead;
  lead.name = "lead";
  Instrument drum;
  drum.name = "snare";
  drum.drum = true;
  drum.volume = {14, 10, 6, 3, 1, 0};
  drum.release.clear();
  drum.drumControl = {8, 8, 8};
  drum.drumFrequency = {2, 4, 6};
  song.instruments = {lead, drum};
  Pattern pattern;
  pattern.name = "intro";
  pattern.resize(32);
  song.patterns.push_back(pattern);
  return song;
}

// Cells are written as a tracker shows them, one string per row: "C-4 00" (note and
// instrument in hex), "=== .." (note off), "--- .." (nothing), so a pattern diffs by row.
static const char* kNames[] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

static std::string cellText(const Cell& cell) {
  std::string note = cell.note == kNoteOff ? "===" : cell.note < 0 ? "---" : std::string(kNames[cell.note % 12]) + std::to_string(cell.note / 12 - 1);
  char instrument[4] = "..";
  if (cell.instrument >= 0) snprintf(instrument, sizeof instrument, "%02X", cell.instrument & 0xff);
  return note + " " + instrument;
}

static bool parseCell(const std::string& text, Cell* cell) {
  *cell = Cell();
  if (text.size() < 3) return false;
  std::string note = text.substr(0, 3);
  if (note == "===") cell->note = kNoteOff;
  else if (note != "---") {
    int index = -1;
    for (int name = 0; name < 12; name++)
      if (note.compare(0, 2, kNames[name]) == 0) index = name;
    if (index < 0 || note[2] < '0' || note[2] > '9') return false;
    cell->note = (note[2] - '0' + 1) * 12 + index;
  }
  if (text.size() >= 6 && text.compare(4, 2, "..") != 0) cell->instrument = (int)std::strtol(text.substr(4, 2).c_str(), nullptr, 16);
  return true;
}

std::string toJson(const Song& song) {
  json instruments = json::array();
  for (auto& instrument : song.instruments) {
    json item = {{"name", instrument.name}, {"volume", instrument.volume}, {"release", instrument.release}};
    if (instrument.loopStart >= 0) item["loop"] = instrument.loopStart;
    if (instrument.drum) {
      item["drum"] = true;
      item["audc"] = instrument.drumControl;
      item["audf"] = instrument.drumFrequency;
    } else {
      item["controls"] = instrument.controls;
      if (!instrument.pitch.empty()) item["pitch"] = instrument.pitch;
    }
    instruments.push_back(item);
  }
  json patterns = json::array();
  for (auto& pattern : song.patterns) {
    json item = {{"name", pattern.name}, {"rows", pattern.rows}};
    json channels = json::array();
    for (auto& channel : pattern.cells) {
      json rows = json::array();
      for (auto& cell : channel) rows.push_back(cellText(cell));
      channels.push_back(rows);
    }
    item["channels"] = channels;
    patterns.push_back(item);
  }
  json tuning = {{"transpose", song.tuning.transpose},
                 {"reference_cents", song.tuning.referenceCents},
                 {"goal", song.tuning.goal == TuningGoal::Average ? "average" : "worst"}};
  json document = {{"format", "vcsmusic"}, {"version", 1},       {"name", song.name},       {"target", "tia"},
                   {"speed", song.speed},  {"swing", song.swing}, {"tuning", tuning},       {"instruments", instruments},
                   {"order", song.order},  {"loop_to", song.loopTo}, {"patterns", patterns}};
  return document.dump(1) + "\n";
}

bool fromJson(const std::string& text, Song* song, std::string* error) {
  try {
    json document = json::parse(text);
    if (!document.is_object() || document.value("format", std::string("vcsmusic")) != "vcsmusic") throw std::runtime_error(T("not a VCSIDE music file"));
    Song parsed;
    parsed.name = document.value("name", parsed.name);
    parsed.speed = std::clamp(document.value("speed", 6), 1, 255);
    parsed.swing = std::clamp(document.value("swing", 0), 0, 255);
    if (document.contains("tuning")) {
      auto& tuning = document["tuning"];
      parsed.tuning.transpose = std::clamp(tuning.value("transpose", 0), -24, 24);
      parsed.tuning.referenceCents = std::clamp(tuning.value("reference_cents", 0), -50, 50);
      parsed.tuning.goal = tuning.value("goal", "worst") == "average" ? TuningGoal::Average : TuningGoal::WorstNote;
    }
    auto clampedList = [](const json& list, int low, int high) {
      std::vector<int> values;
      if (list.is_array())
        for (auto& value : list)
          if (value.is_number_integer()) values.push_back(std::clamp(value.get<int>(), low, high));
      return values;
    };
    for (auto& item : document.value("instruments", json::array())) {
      Instrument instrument;
      instrument.name = item.value("name", instrument.name);
      instrument.drum = item.value("drum", false);
      instrument.volume = clampedList(item.value("volume", json::array()), 0, 15);
      instrument.release = clampedList(item.value("release", json::array()), 0, 15);
      instrument.loopStart = std::clamp(item.value("loop", -1), -1, std::max(-1, (int)instrument.volume.size() - 1));
      if (instrument.drum) {
        instrument.drumControl = clampedList(item.value("audc", json::array()), 0, 15);
        instrument.drumFrequency = clampedList(item.value("audf", json::array()), 0, 31);
      } else {
        instrument.controls = clampedList(item.value("controls", json::array()), 0, 15);
        instrument.pitch = clampedList(item.value("pitch", json::array()), -31, 31);
      }
      parsed.instruments.push_back(instrument);
    }
    for (auto& item : document.value("patterns", json::array())) {
      Pattern pattern;
      pattern.name = item.value("name", "");
      pattern.resize(item.value("rows", 32));
      auto channels = item.value("channels", json::array());
      for (int channel = 0; channel < kChannels && channel < (int)channels.size(); channel++)
        for (int row = 0; row < pattern.rows && row < (int)channels[channel].size(); row++) {
          if (!channels[channel][row].is_string() || !parseCell(channels[channel][row].get<std::string>(), &pattern.cells[channel][row]))
            throw std::runtime_error(F("pattern \"{0}\", channel {1}, row {2}: not a cell", {pattern.name, std::to_string(channel), std::to_string(row)}));
        }
      parsed.patterns.push_back(pattern);
    }
    if (parsed.patterns.empty()) {
      Pattern pattern;
      pattern.resize(32);
      parsed.patterns.push_back(pattern);
    }
    parsed.order.clear();
    for (int index : clampedList(document.value("order", json::array()), 0, (int)parsed.patterns.size() - 1)) parsed.order.push_back(index);
    if (parsed.order.empty()) parsed.order.push_back(0);
    parsed.loopTo = std::clamp(document.value("loop_to", 0), -1, (int)parsed.order.size() - 1);
    *song = std::move(parsed);
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
  return true;
}

}  // namespace vcs::music
