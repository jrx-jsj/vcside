// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "import_tiatracker.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <numeric>
#include <set>

#include "../i18n.h"

namespace vcs::music {

using nlohmann::json;

namespace {

constexpr int kPureCombined = 16;  // a waveform of TIATracker: AUDC 4 for values 0-31, 12 for 32-63
constexpr int kMaxTimelineRows = 8192;

enum NoteType { Hold = 0, Play = 1, Pause = 2, Percussion = 3, Slide = 4 };

// The AUDC a TIATracker waveform sounds as: some AUDC values give the same sound, and
// TIATracker plays them as the first of each (11 is silent). -1: silent.
int controlOf(int waveform) {
  switch (waveform) {
    case 5: return 4;
    case 9: return 7;
    case 10: return 6;
    case 11: return -1;
    case 13: return 12;
    default: return std::clamp(waveform, 0, 15);
  }
}

struct TrackerInstrument {
  std::string name;
  int waveform = 4;
  int sustainStart = 0, releaseStart = 1;
  std::vector<int> frequencies, volumes;
};

struct TrackerPercussion {
  std::string name;
  bool overlay = false;
  std::vector<int> frequencies, volumes, waveforms;
};

struct TrackerNote {
  int type = Hold, number = 0, value = 0;
};

struct TrackerPattern {
  std::string name;
  int evenSpeed = 3, oddSpeed = 3;
  std::vector<TrackerNote> notes;
};

// One channel played from its start: its rows in order, where each pattern starts, and the
// row it goes back to at the end (-1: it stops).
struct ChannelTimeline {
  std::vector<TrackerNote> rows;
  std::vector<int> patternOfRow;  // index in the song's patterns
  std::vector<int> rowInPattern;
  std::set<int> patternStarts;
  int loopRow = -1;
};

std::vector<int> intArray(const json& object, const char* key) {
  std::vector<int> values;
  if (object.contains(key) && object[key].is_array())
    for (auto& value : object[key]) values.push_back(value.is_number() ? value.get<int>() : 0);
  return values;
}

ChannelTimeline layOut(const json& channel, int start, const std::vector<TrackerPattern>& patterns) {
  ChannelTimeline timeline;
  if (!channel.contains("sequence") || !channel["sequence"].is_array()) return timeline;
  const json& sequence = channel["sequence"];
  int count = (int)sequence.size();
  std::map<int, int> rowOfEntry;  // entries already laid out, and their first row
  int entry = std::clamp(start, 0, std::max(count - 1, 0));
  while (entry >= 0 && entry < count) {
    if (rowOfEntry.count(entry)) {
      timeline.loopRow = rowOfEntry[entry];
      break;
    }
    rowOfEntry[entry] = (int)timeline.rows.size();
    int patternIndex = sequence[entry].value("patternindex", 0);
    int target = sequence[entry].value("gototarget", -1);
    if (patternIndex >= 0 && patternIndex < (int)patterns.size() && !patterns[patternIndex].notes.empty()) {
      timeline.patternStarts.insert((int)timeline.rows.size());
      for (size_t row = 0; row < patterns[patternIndex].notes.size(); row++)
        timeline.rows.push_back(patterns[patternIndex].notes[row]), timeline.patternOfRow.push_back(patternIndex), timeline.rowInPattern.push_back((int)row);
    }
    entry = target >= 0 && target < count ? target : entry + 1;
  }
  return timeline;
}

// The note of channel row `row` of the combined song (Hold past a channel's end).
const TrackerNote* rowAt(const ChannelTimeline& timeline, int row) {
  int length = (int)timeline.rows.size();
  if (row < length) return &timeline.rows[row];
  if (timeline.loopRow < 0 || timeline.loopRow >= length) return nullptr;
  return &timeline.rows[timeline.loopRow + (row - timeline.loopRow) % (length - timeline.loopRow)];
}

// The timeline's own row for row `row` of the combined song (-1 past its end).
int timelineRow(const ChannelTimeline& timeline, int row) {
  int length = (int)timeline.rows.size();
  if (row < length) return row;
  if (timeline.loopRow < 0 || timeline.loopRow >= length) return -1;
  return timeline.loopRow + (row - timeline.loopRow) % (length - timeline.loopRow);
}

int patternAt(const ChannelTimeline& timeline, int row) {
  int own = timelineRow(timeline, row);
  return own < 0 ? -1 : timeline.patternOfRow[own];
}

bool startsPattern(const ChannelTimeline& timeline, int row) {
  int length = (int)timeline.rows.size();
  if (row < length) return timeline.patternStarts.count(row) > 0;
  if (timeline.loopRow < 0 || timeline.loopRow >= length) return row == length;
  return timeline.patternStarts.count(timeline.loopRow + (row - timeline.loopRow) % (length - timeline.loopRow)) > 0;
}

bool sameCells(const Pattern& a, const Pattern& b) {
  if (a.rows != b.rows || a.speed != b.speed || a.swing != b.swing) return false;
  for (int channel = 0; channel < kChannels; channel++)
    for (int row = 0; row < a.rows; row++) {
      const Cell &x = a.cells[channel][row], &y = b.cells[channel][row];
      if (x.note != y.note || x.instrument != y.instrument || x.effect != y.effect || x.effectValue != y.effectValue) return false;
    }
  return true;
}

}  // namespace

ImportResult importTiaTracker(const std::string& text, const std::string& fallbackName, Song* song) {
  ImportResult result;
  json root = json::parse(text, nullptr, false);
  if (root.is_discarded() || !root.is_object() || !root.contains("instruments") || !root.contains("channels") || !root.contains("patterns")) {
    result.error = T("This is not a TIATracker song (.ttt).");
    return result;
  }
  std::set<std::string> warned;
  auto warn = [&](const std::string& message) {
    if (warned.insert(message).second) result.warnings.push_back(message);
  };
  std::string tv = root.value("tvmode", std::string("ntsc")) == "pal" ? "PAL" : "NTSC";
  double clock = audioClockHz(tv);

  std::vector<TrackerInstrument> instruments;
  for (auto& item : root["instruments"]) {
    TrackerInstrument instrument;
    instrument.name = item.value("name", std::string("instrument"));
    instrument.waveform = item.value("waveform", 4);
    instrument.frequencies = intArray(item, "frequencies");
    instrument.volumes = intArray(item, "volumes");
    int length = std::min((int)instrument.volumes.size(), item.value("envelopeLength", (int)instrument.volumes.size()));
    instrument.volumes.resize(std::max(length, 2), 0);
    instrument.frequencies.resize(instrument.volumes.size(), 0);
    length = (int)instrument.volumes.size();
    instrument.sustainStart = std::clamp(item.value("sustainStart", 0), 0, length - 2);
    instrument.releaseStart = std::clamp(item.value("releaseStart", length - 1), instrument.sustainStart + 1, length - 1);
    instruments.push_back(instrument);
  }
  std::vector<TrackerPercussion> percussion;
  if (root.contains("percussion"))
    for (auto& item : root["percussion"]) {
      TrackerPercussion sound;
      sound.name = item.value("name", std::string("percussion"));
      sound.overlay = item.value("overlay", false);
      sound.frequencies = intArray(item, "frequencies");
      sound.volumes = intArray(item, "volumes");
      sound.waveforms = intArray(item, "waveforms");
      int length = std::min({(int)sound.volumes.size(), (int)sound.frequencies.size(), (int)sound.waveforms.size()});
      length = std::min(length, item.value("envelopeLength", length));
      sound.volumes.resize(std::max(length, 1), 0), sound.frequencies.resize(std::max(length, 1), 0), sound.waveforms.resize(std::max(length, 1), 8);
      percussion.push_back(sound);
    }
  std::vector<TrackerPattern> patterns;
  for (auto& item : root["patterns"]) {
    TrackerPattern pattern;
    pattern.name = item.value("name", std::string());
    pattern.evenSpeed = item.value("evenspeed", 3), pattern.oddSpeed = item.value("oddspeed", 3);
    if (item.contains("notes"))
      for (auto& noteItem : item["notes"]) pattern.notes.push_back({noteItem.value("type", 0), noteItem.value("number", 0), noteItem.value("value", 0)});
    patterns.push_back(pattern);
  }
  if (!root["channels"].is_array() || root["channels"].size() < 2) {
    result.error = T("This is not a TIATracker song (.ttt).");
    return result;
  }
  ChannelTimeline timelines[kChannels] = {layOut(root["channels"][0], root.value("startpattern0", 0), patterns),
                                          layOut(root["channels"][1], root.value("startpattern1", 0), patterns)};

  // How long the two channels play together, and where that loops.
  int lengths[kChannels], intros[kChannels], loops[kChannels];
  bool bothLoop = true;
  for (int channel = 0; channel < kChannels; channel++) {
    lengths[channel] = (int)timelines[channel].rows.size();
    bool loopsBack = timelines[channel].loopRow >= 0 && timelines[channel].loopRow < lengths[channel];
    intros[channel] = loopsBack ? timelines[channel].loopRow : lengths[channel];
    loops[channel] = loopsBack ? lengths[channel] - timelines[channel].loopRow : 0;
    bothLoop &= loopsBack;
  }
  // TIATracker stops when a channel that does not loop runs out of notes
  int totalRows = std::max(lengths[0], lengths[1]), loopRow = -1;
  for (int channel = 0; channel < kChannels; channel++)
    if (!(timelines[channel].loopRow >= 0 && timelines[channel].loopRow < lengths[channel])) totalRows = std::min(totalRows, lengths[channel]);
  if (bothLoop) {
    loopRow = std::max(intros[0], intros[1]);
    long long together = std::lcm((long long)loops[0], (long long)loops[1]);
    if (loopRow + together <= kMaxTimelineRows) {
      totalRows = loopRow + (int)together;
    } else {
      totalRows = std::max(loopRow + loops[0], loopRow + loops[1]);
      warn(T("The two channels loop with lengths that take too long to meet again: the imported song loops where the longer one ends."));
    }
  }
  if (totalRows == 0) {
    result.error = T("The song has no notes.");
    return result;
  }

  // Speed: TIATracker reads it from channel 0's pattern (or the song's, if global), and
  // alternates the even and odd speeds by channel 0's row in its pattern.
  bool globalSpeed = root.value("globalspeed", true);
  int songEven = root.value("evenspeed", 6), songOdd = root.value("oddspeed", 6);
  // even and odd frames of a combined row that starts a pattern of VCSIDE's
  auto speedsAt = [&](int row) {
    // TIATracker times a row by the row after it: rows 0, 2, 4... last "oddspeed" frames
    int even = songOdd, odd = songEven;
    int own = timelineRow(timelines[0], row);
    if (!globalSpeed && own >= 0) even = patterns[timelines[0].patternOfRow[own]].oddSpeed, odd = patterns[timelines[0].patternOfRow[own]].evenSpeed;
    if (own >= 0 && timelines[0].rowInPattern[own] % 2 == 1) std::swap(even, odd);
    return std::make_pair(std::clamp(even, 1, 255), std::clamp(odd, 1, 255));
  };
  // Instruments, as they are used: TIATracker's melodic ones first, then its percussion.
  Song imported;
  imported.name = root.value("metaName", std::string());
  if (imported.name.empty()) imported.name = fallbackName;
  auto [firstEven, firstOdd] = speedsAt(0);
  imported.speed = firstEven;
  imported.swing = firstOdd != firstEven ? firstOdd : 0;
  imported.loopTo = -1;
  std::map<int, int> instrumentIndex, percussionIndex;
  // per imported instrument: (AUDC, AUDF) -> note, and the notes taken
  std::map<int, std::map<std::pair<int, int>, int>> noteOf;
  std::map<int, std::set<int>> notesTaken;

  auto melodic = [&](int number) -> int {
    auto found = instrumentIndex.find(number);
    if (found != instrumentIndex.end()) return found->second;
    const TrackerInstrument& source = instruments[number];
    Instrument instrument;
    instrument.name = source.name;
    instrument.drum = false;
    int control = controlOf(source.waveform);
    if (source.waveform == kPureCombined) instrument.controls = {4, 12};
    else if (control >= 0) instrument.controls = {control};
    else instrument.controls = {};
    std::vector<int> held(source.volumes.begin(), source.volumes.begin() + source.releaseStart);
    if (control < 0 && source.waveform != kPureCombined) held.assign(held.size(), 0);
    instrument.volumes = {{held, source.sustainStart}};
    std::vector<int> release(source.volumes.begin() + source.releaseStart, source.volumes.end());
    while (!release.empty() && release.back() == 0) release.pop_back();
    instrument.releases = {release};
    instrument.pitch.clear();
    bool loopBends = false;
    for (int frame = source.sustainStart + 1; frame < source.releaseStart; frame++) loopBends |= source.frequencies[frame] != source.frequencies[source.sustainStart];
    if (loopBends) warn(T("An instrument bends its pitch while it is held: the bend is written out for 120 frames, then the last value stays."));
    bool releaseBends = false;
    for (int frame = source.releaseStart; frame < (int)source.frequencies.size(); frame++)
      releaseBends |= source.frequencies[frame] != source.frequencies[source.releaseStart - 1];
    if (releaseBends) warn(T("An instrument bends its pitch in its release: the release keeps the pitch the note had."));
    imported.instruments.push_back(instrument);
    return instrumentIndex[number] = (int)imported.instruments.size() - 1;
  };
  auto drum = [&](int number) -> int {
    auto found = percussionIndex.find(number);
    if (found != percussionIndex.end()) return found->second;
    const TrackerPercussion& source = percussion[number];
    Instrument instrument;
    instrument.name = source.name;
    instrument.drum = true;
    std::vector<int> volumes = source.volumes;
    if (volumes.back() != 0) volumes.push_back(0);  // it plays once and stops
    instrument.volumes = {{volumes, -1}};
    instrument.releases = {{}};
    instrument.drumControl.clear();
    for (int waveform : source.waveforms) {
      int control = controlOf(waveform);
      instrument.drumControl.push_back(control < 0 ? 0 : control);
    }
    instrument.drumFrequency = source.frequencies;
    for (auto& frequency : instrument.drumFrequency) frequency = std::clamp(frequency, 0, 31);
    if (source.overlay) warn(T("Overlay percussion is played as plain percussion: the next note starts on its own row."));
    imported.instruments.push_back(instrument);
    return percussionIndex[number] = (int)imported.instruments.size() - 1;
  };
  // The note an instrument's AUDF plays as: the closest musical note, tuned by hand to it.
  auto noteFor = [&](int number, int value) -> int {
    int index = melodic(number);
    const TrackerInstrument& source = instruments[number];
    int control = controlOf(source.waveform), frequency = value & 31;
    if (source.waveform == kPureCombined) control = value >= 32 ? 12 : 4;
    if (control < 0) return 60;
    auto key = std::make_pair(control, frequency);
    auto found = noteOf[index].find(key);
    if (found != noteOf[index].end()) return found->second;
    int note = 60;
    if (const Distortion* distortion = distortionOf(control))
      note = std::clamp((int)std::lround(noteNumber(pitchHz(clock, distortion->period, frequency))), 12, 119);
    std::set<int>& taken = notesTaken[index];
    for (int step = 0; taken.count(note); step++) {  // two AUDF values near one note: the next free one
      int candidate = note + ((step % 2 == 0) ? (step / 2 + 1) : -(step / 2 + 1));
      if (candidate >= 0 && candidate < 128 && !taken.count(candidate)) {
        note = candidate;
        warn(T("Two AUDF values of one instrument are closest to the same note: one of them is written as a neighbouring note (it still sounds as before)."));
        break;
      }
    }
    taken.insert(note);
    noteOf[index][key] = note;
    // its AUDC and AUDF frame by frame, with the envelope's offsets (the held loop written out)
    NoteTuning tuning;
    tuning.control = control;
    bool bends = false, loopBends = false;
    for (int frame = 0; frame < source.releaseStart; frame++) bends |= source.frequencies[frame] != 0;
    for (int frame = source.sustainStart + 1; frame < source.releaseStart; frame++) loopBends |= source.frequencies[frame] != source.frequencies[source.sustainStart];
    int frames = bends ? (loopBends ? 120 : source.releaseStart) : 1, loopLength = source.releaseStart - source.sustainStart;
    for (int frame = 0; frame < frames; frame++) {
      int envelopeFrame = frame < source.releaseStart ? frame : source.sustainStart + (frame - source.sustainStart) % loopLength;
      tuning.frequencies.push_back(std::clamp(frequency + (bends ? source.frequencies[envelopeFrame] : 0), 0, 31));
    }
    if (!loopBends)  // the last value stays anyway
      while (tuning.frequencies.size() > 1 && tuning.frequencies.back() == tuning.frequencies[tuning.frequencies.size() - 2]) tuning.frequencies.pop_back();
    imported.instruments[index].byHand[note] = tuning;
    return note;
  };

  // Each channel's cells, row by row, following its slides.
  std::vector<Cell> cells[kChannels];
  for (int channel = 0; channel < kChannels; channel++) {
    int currentInstrument = -1, currentValue = 0;
    bool percussionLast = false;  // TIATracker's pauses only release instruments
    for (int row = 0; row < totalRows; row++) {
      Cell cell;
      const TrackerNote* note = rowAt(timelines[channel], row);
      if (!note) {
        if (row == lengths[channel] && row > 0) cell.note = kNoteOff;  // the channel has ended
      } else if (note->type == Play && note->number >= 0 && note->number < (int)instruments.size()) {
        currentInstrument = note->number, currentValue = note->value, percussionLast = false;
        cell.note = noteFor(note->number, note->value);
        cell.instrument = melodic(note->number);
      } else if (note->type == Pause) {
        if (!percussionLast) cell.note = kNoteOff;
      } else if (note->type == Percussion && note->number >= 0 && note->number < (int)percussion.size()) {
        currentInstrument = -1, percussionLast = true;
        cell.note = 60;
        cell.instrument = drum(note->number);
      } else if (note->type == Slide && currentInstrument >= 0) {
        warn(T("Slides become new notes: the slid note starts its envelope again."));
        int top = instruments[currentInstrument].waveform == kPureCombined ? 63 : 31;
        currentValue = std::clamp(currentValue + note->value, 0, top);
        cell.note = noteFor(currentInstrument, currentValue);
        cell.instrument = melodic(currentInstrument);
      }
      cells[channel].push_back(cell);
    }
  }

  // Cut into patterns where either channel starts one (and at the loop and every 256 rows).
  std::vector<int> cuts;
  for (int row = 0; row < totalRows; row++)
    if (row == 0 || row == loopRow || startsPattern(timelines[0], row) || startsPattern(timelines[1], row) || row - cuts.back() >= 256) cuts.push_back(row);
  cuts.push_back(totalRows);
  imported.order.clear();
  for (size_t piece = 0; piece + 1 < cuts.size(); piece++) {
    Pattern pattern;
    pattern.resize(cuts[piece + 1] - cuts[piece]);
    auto [even, odd] = speedsAt(cuts[piece]);
    if (even != imported.rowFrames(0) || odd != imported.rowFrames(1)) pattern.speed = even, pattern.swing = odd != even ? odd : 0;
    for (int channel = 0; channel < kChannels; channel++)
      for (int row = 0; row < pattern.rows; row++) pattern.cells[channel][row] = cells[channel][cuts[piece] + row];
    std::string names[kChannels];
    for (int channel = 0; channel < kChannels; channel++) {
      int source = patternAt(timelines[channel], cuts[piece]);
      names[channel] = source >= 0 ? patterns[source].name : std::string();
    }
    pattern.name = names[0] == names[1] || names[1].empty() ? names[0] : names[0].empty() ? names[1] : names[0] + " + " + names[1];
    if (cuts[piece] == loopRow) imported.loopTo = (int)imported.order.size();
    int index = -1;
    for (size_t existing = 0; existing < imported.patterns.size(); existing++)
      if (sameCells(imported.patterns[existing], pattern)) index = (int)existing;
    if (index < 0) imported.patterns.push_back(pattern), index = (int)imported.patterns.size() - 1;
    imported.order.push_back(index);
  }
  if (imported.order.size() > 254) warn(T("The song has more than 254 patterns in its order list: it cannot be exported until it is shorter."));
  if (imported.instruments.empty()) imported.instruments.push_back(Instrument{});
  *song = imported;
  result.ok = true;
  return result;
}

}  // namespace vcs::music
