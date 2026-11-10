// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "song.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    for (int channel = 0; channel < channelCount(); channel++) {
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

std::vector<int> Song::notesOf(int instrument) const {
  std::vector<int> heard;
  for (int orderIndex : order) {
    if (orderIndex < 0 || orderIndex >= (int)patterns.size()) continue;
    for (int channel = 0; channel < channelCount(); channel++) {
      int instrumentIndex = 0;
      for (const Cell& cell : patterns[orderIndex].cells[channel]) {
        if (cell.instrument >= 0) instrumentIndex = cell.instrument;
        if (cell.note >= 0 && instrumentIndex == instrument) heard.push_back(cell.note + tuning.transpose);
      }
    }
  }
  std::sort(heard.begin(), heard.end());
  heard.erase(std::unique(heard.begin(), heard.end()), heard.end());
  return heard;
}

std::array<uint8_t, 32> waveLevels(const Instrument& instrument) {
  std::array<uint8_t, 32> levels{};
  for (int step = 0; step < 32; step++) {
    int level = 0;
    switch (instrument.wave) {
      case Waveform::Square: level = step < std::clamp(instrument.duty, 1, 31) ? 15 : 0; break;
      case Waveform::Triangle: level = step < 16 ? step : 31 - step; break;
      case Waveform::Saw: level = step / 2; break;
      case Waveform::Sine: level = (int)std::lround(7.5 + 7.5 * std::sin(2 * 3.14159265358979323846 * step / 32)); break;
      case Waveform::Noise: level = 0; break;
      case Waveform::Drawn: level = step < (int)instrument.drawn.size() ? instrument.drawn[step] : 0; break;
    }
    levels[step] = (uint8_t)std::clamp(level, 0, 15);
  }
  return levels;
}

int noiseMask(int bits) {
  switch (bits) {
    case 4: return 0x9;      // x^4 + x^3 + 1
    case 5: return 0x12;     // x^5 + x^3 + 1
    case 9: return 0x110;    // x^9 + x^5 + 1
    default: return 0x6000;  // x^15 + x^14 + 1
  }
}

std::vector<uint32_t> Song::phaseSteps(int note, const Instrument& instrument, double audioClock) const {
  double lineRate = audioClock / 2;  // two audio clocks per scanline
  auto stepFor = [&](int cents) {
    double semitones = note + tuning.transpose - 69 + (tuning.referenceCents + cents) / 100.0;
    double hz = 440.0 * std::pow(2.0, semitones / 12.0);
    return (uint32_t)std::clamp(std::llround(hz / lineRate * 4294967296.0), 0LL, 0x7fffffffLL);
  };
  std::vector<uint32_t> steps;
  for (int cents : instrument.pitch) steps.push_back(stepFor(cents));
  if (steps.empty()) steps.push_back(stepFor(0));
  return steps;
}

// How many cents a register pair is from a (fractional) note.
static double centsFrom(double audioClock, int control, int frequency, double note, int referenceCents) {
  const Distortion* distortion = distortionOf(control);
  if (!distortion) return 0;
  return (noteNumber(pitchHz(audioClock, distortion->period, frequency), referenceCents) - note) * 100.0;
}

NotePitch Song::pitchOf(int note, const Instrument& instrument, double audioClock) const {
  int heard = note + tuning.transpose;
  if (samples()) {  // the ARM plays any pitch: every note is exact
    NotePitch exact;
    exact.note = note, exact.playable = true;
    exact.hz = 440.0 * std::pow(2.0, (heard - 69 + tuning.referenceCents / 100.0) / 12.0);
    return exact;
  }
  auto byHand = instrument.byHand.find(heard);
  if (byHand != instrument.byHand.end() && distortionOf(byHand->second.control)) {
    NotePitch pitch;
    pitch.note = note;
    pitch.playable = true;
    pitch.control = byHand->second.control;
    pitch.frequency = byHand->second.frequencies.empty() ? 0 : byHand->second.frequencies[0];
    pitch.hz = pitchHz(audioClock, distortionOf(pitch.control)->period, pitch.frequency);
    pitch.cents = centsFrom(audioClock, pitch.control, pitch.frequency, heard, tuning.referenceCents);
    return pitch;
  }
  NotePitch pitch = closestPitch(heard, audioClock, instrument.controls, tuning.referenceCents);
  pitch.note = note;
  return pitch;
}

std::vector<PitchStep> Song::pitchSequence(int note, const Instrument& instrument, double audioClock) const {
  int heard = note + tuning.transpose;
  std::vector<PitchStep> steps;
  auto byHand = instrument.byHand.find(heard);
  if (byHand != instrument.byHand.end() && distortionOf(byHand->second.control)) {
    for (int frequency : byHand->second.frequencies) {
      PitchStep step;
      step.playable = true, step.control = byHand->second.control, step.frequency = std::clamp(frequency, 0, 31);
      step.cents = centsFrom(audioClock, step.control, step.frequency, heard, tuning.referenceCents);
      steps.push_back(step);
    }
    if (steps.empty()) steps.push_back(PitchStep{true, byHand->second.control, 0, centsFrom(audioClock, byHand->second.control, 0, heard, tuning.referenceCents)});
    return steps;
  }
  NotePitch base = pitchOf(note, instrument, audioClock);
  PitchStep first{base.playable, base.control, base.frequency, base.cents};
  if (!base.playable || instrument.pitch.empty()) return {first};
  for (int offset : instrument.pitch) {
    PitchStep step = first;
    if (instrument.pitchUnit == PitchUnit::Audf) {
      step.frequency = std::clamp(base.frequency + offset, 0, 31);
      step.cents = centsFrom(audioClock, step.control, step.frequency, heard, tuning.referenceCents);
    } else {
      // the AUDF of the note's own AUDC that comes closest to the note moved by these cents
      double target = heard + offset / 100.0;
      double bestDistance = 1e9;
      for (int frequency = 0; frequency < 32; frequency++) {
        double cents = centsFrom(audioClock, step.control, frequency, target, tuning.referenceCents);
        if (std::fabs(cents) < bestDistance) bestDistance = std::fabs(cents), step.frequency = frequency, step.cents = cents;
      }
    }
    steps.push_back(step);
  }
  return steps;
}

bool Song::patternSpeeds() const {
  for (auto& pattern : patterns)
    if (pattern.speed > 0) return true;
  return false;
}

Song blankSong() {
  Song song;
  Instrument lead;
  lead.name = "lead";
  Instrument drum;
  drum.name = "snare";
  drum.drum = true;
  drum.volumes = {{{14, 10, 6, 3, 1, 0}, -1}};
  drum.releases = {{}};
  drum.drumControl = {8, 8, 8};
  drum.drumFrequency = {2, 4, 6};
  song.instruments = {lead, drum};
  Pattern pattern;
  pattern.name = "intro";
  pattern.resize(32);
  song.patterns.push_back(pattern);
  return song;
}

// Cells are written as a tracker shows them, one string per row: "C-4 00 V1" (note,
// instrument in hex, effect), "=== .. .." (note off), "--- .. .." (nothing), so a
// pattern diffs by row. Version 1 files had no effect.
static const char* kNames[] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

static std::string cellText(const Cell& cell) {
  std::string note = cell.note == kNoteOff ? "===" : cell.note < 0 ? "---" : std::string(kNames[cell.note % 12]) + std::to_string(cell.note / 12 - 1);
  char instrument[4] = "..";
  if (cell.instrument >= 0) snprintf(instrument, sizeof instrument, "%02X", cell.instrument & 0xff);
  std::string effect = "..";
  if (cell.effect) {
    static const char* digits = "0123456789ABCDEF";
    effect = std::string(1, cell.effect) + digits[cell.effectValue & 15];
  }
  return note + " " + instrument + " " + effect;
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
  if (text.size() >= 9 && text.compare(7, 2, "..") != 0) {
    char letter = text[7];
    if (letter != kEffectVolume && letter != kEffectRelease) return false;
    char* end = nullptr;
    long value = std::strtol(text.substr(8, 1).c_str(), &end, 16);
    if (!end || *end) return false;
    cell->effect = letter, cell->effectValue = (int)value;
  }
  return true;
}

std::string toJson(const Song& song) {
  json instruments = json::array();
  for (auto& instrument : song.instruments) {
    json volumes = json::array();
    for (auto& envelope : instrument.volumes) {
      json entry = {{"values", envelope.values}};
      if (envelope.loop >= 0) entry["loop"] = envelope.loop;
      volumes.push_back(entry);
    }
    json item = {{"name", instrument.name}, {"volumes", volumes}, {"releases", instrument.releases}};
    if (song.samples()) {
      static const char* waves[] = {"square", "triangle", "saw", "sine", "noise", "drawn"};
      item["wave"] = waves[(int)instrument.wave];
      if (instrument.wave == Waveform::Square) item["duty"] = instrument.duty;
      if (instrument.wave == Waveform::Noise) item["noise_bits"] = instrument.noiseBits;
      if (instrument.wave == Waveform::Drawn) item["drawn"] = instrument.drawn;
    }
    if (instrument.drum) {
      item["drum"] = true;
      item["audc"] = instrument.drumControl;
      item["audf"] = instrument.drumFrequency;
    } else {
      item["controls"] = instrument.controls;
      if (!instrument.pitch.empty()) {
        item["pitch"] = instrument.pitch;
        item["pitch_unit"] = instrument.pitchUnit == PitchUnit::Audf ? "audf" : "cents";
      }
      if (!instrument.byHand.empty()) {
        json byHand = json::array();
        for (auto& [note, tuned] : instrument.byHand) byHand.push_back({{"note", note}, {"audc", tuned.control}, {"audf", tuned.frequencies}});
        item["by_hand"] = byHand;
      }
    }
    instruments.push_back(item);
  }
  json patterns = json::array();
  for (auto& pattern : song.patterns) {
    json item = {{"name", pattern.name}, {"rows", pattern.rows}};
    if (pattern.speed > 0) item["speed"] = pattern.speed, item["swing"] = pattern.swing;
    json channels = json::array();
    for (int channel = 0; channel < song.channelCount(); channel++) {
      json rows = json::array();
      for (auto& cell : pattern.cells[channel]) rows.push_back(cellText(cell));
      channels.push_back(rows);
    }
    item["channels"] = channels;
    patterns.push_back(item);
  }
  json tuning = {{"transpose", song.tuning.transpose},
                 {"reference_cents", song.tuning.referenceCents},
                 {"goal", song.tuning.goal == TuningGoal::Average ? "average" : "worst"}};
  json effects = json::array();
  for (auto& effect : song.effects)
    effects.push_back({{"name", effect.name}, {"instrument", effect.instrument}, {"note", effect.note}, {"channel", effect.channel}});
  json document = {{"format", "vcsmusic"}, {"version", 2},       {"name", song.name},       {"target", song.samples() ? "samples" : "tia"},
                   {"speed", song.speed},  {"swing", song.swing}, {"tuning", tuning},       {"instruments", instruments},
                   {"order", song.order},  {"loop_to", song.loopTo}, {"patterns", patterns}};
  if (song.samples()) document["voices"] = song.channelCount(), document["gain"] = song.gain;
  if (!song.effects.empty()) document["effects"] = effects;
  if (!song.exportTo.empty()) document["export"] = {{"to", song.exportTo}, {"ram", song.exportRam}};
  return document.dump(1) + "\n";
}

bool fromJson(const std::string& text, Song* song, std::string* error) {
  try {
    json document = json::parse(text);
    if (!document.is_object() || document.value("format", std::string("vcsmusic")) != "vcsmusic") throw std::runtime_error(T("not a VCSIDE music file"));
    int version = document.value("version", 1);
    Song parsed;
    parsed.name = document.value("name", parsed.name);
    parsed.speed = std::clamp(document.value("speed", 6), 1, 255);
    parsed.swing = std::clamp(document.value("swing", 0), 0, 255);
    parsed.target = document.value("target", "tia") == "samples" ? "samples" : "tia";
    parsed.voices = std::clamp(document.value("voices", 3), 1, kMaxChannels);
    parsed.gain = std::clamp(document.value("gain", 16), 4, 64);
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
      auto envelopeFrom = [&](const json& values, int loop) {
        Envelope envelope;
        envelope.values = clampedList(values, 0, 15);
        envelope.loop = std::clamp(loop, -1, std::max(-1, (int)envelope.values.size() - 1));
        return envelope;
      };
      instrument.volumes.clear(), instrument.releases.clear();
      if (item.contains("volumes")) {
        for (auto& entry : item["volumes"])
          if (entry.is_object()) instrument.volumes.push_back(envelopeFrom(entry.value("values", json::array()), entry.value("loop", -1)));
        for (auto& entry : item.value("releases", json::array())) instrument.releases.push_back(clampedList(entry, 0, 15));
      } else {  // version 1: one of each
        instrument.volumes.push_back(envelopeFrom(item.value("volume", json::array()), item.value("loop", -1)));
        instrument.releases.push_back(clampedList(item.value("release", json::array()), 0, 15));
      }
      if (instrument.volumes.empty()) instrument.volumes.push_back(Envelope{{15}, -1});
      if (instrument.releases.empty()) instrument.releases.push_back({});
      if (instrument.drum) {
        instrument.drumControl = clampedList(item.value("audc", json::array()), 0, 15);
        instrument.drumFrequency = clampedList(item.value("audf", json::array()), 0, 31);
      } else {
        instrument.controls = clampedList(item.value("controls", json::array()), 0, 15);
        // version 1 had the pitch in AUDF steps only
        bool inSteps = item.value("pitch_unit", version < 2 ? "audf" : "cents") == "audf";
        instrument.pitchUnit = inSteps ? PitchUnit::Audf : PitchUnit::Cents;
        instrument.pitch = clampedList(item.value("pitch", json::array()), inSteps ? -31 : -1200, inSteps ? 31 : 1200);
        for (auto& entry : item.value("by_hand", json::array())) {
          if (!entry.is_object() || !entry.value("note", json()).is_number_integer()) continue;
          NoteTuning tuned;
          tuned.control = std::clamp(entry.value("audc", 4), 0, 15);
          tuned.frequencies = clampedList(entry.value("audf", json::array()), 0, 31);
          instrument.byHand[std::clamp(entry["note"].get<int>(), 0, 127)] = tuned;
        }
      }
      static const char* waves[] = {"square", "triangle", "saw", "sine", "noise", "drawn"};
      std::string wave = item.value("wave", "square");
      for (int index = 0; index < 6; index++)
        if (wave == waves[index]) instrument.wave = (Waveform)index;
      instrument.duty = std::clamp(item.value("duty", 16), 1, 31);
      int bits = item.value("noise_bits", 15);
      instrument.noiseBits = bits == 4 || bits == 5 || bits == 9 ? bits : 15;
      instrument.drawn = clampedList(item.value("drawn", json::array()), 0, 15);
      instrument.drawn.resize(32, 0);
      parsed.instruments.push_back(instrument);
    }
    for (auto& item : document.value("patterns", json::array())) {
      Pattern pattern;
      pattern.name = item.value("name", "");
      pattern.resize(item.value("rows", 32));
      pattern.speed = std::clamp(item.value("speed", 0), 0, 255);
      pattern.swing = pattern.speed > 0 ? std::clamp(item.value("swing", 0), 0, 255) : 0;
      auto channels = item.value("channels", json::array());
      for (int channel = 0; channel < kMaxChannels && channel < (int)channels.size(); channel++)
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
    if (document.contains("export") && document["export"].is_object()) {
      parsed.exportTo = document["export"].value("to", "");
      static const char* targets[] = {"dasm", "bb", "c", "dpc", "cdfj"};
      if (std::find(std::begin(targets), std::end(targets), parsed.exportTo) == std::end(targets)) parsed.exportTo.clear();
      parsed.exportRam = document["export"].value("ram", "h");
    }
    for (auto& item : document.value("effects", json::array())) {
      if (!item.is_object()) continue;
      SoundEffect effect;
      effect.name = item.value("name", effect.name);
      effect.instrument = std::max(0, item.value("instrument", 0));
      effect.note = std::clamp(item.value("note", 60), 0, 127);
      effect.channel = std::clamp(item.value("channel", 1), 0, parsed.channelCount() - 1);
      parsed.effects.push_back(effect);
    }
    *song = std::move(parsed);
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
  return true;
}

}  // namespace vcs::music
