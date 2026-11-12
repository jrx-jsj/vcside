// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "gfx.h"
#include "editor/indentation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>

#include "i18n.h"

using json = nlohmann::json;

namespace vcs::gfx {

// ---------------------------------------------------------------------------
// Colours

Tv tvFromProject(const std::string& tv) {
  if (tv.rfind("PAL", 0) == 0 && tv != "PAL-M") return Tv::PAL;
  if (tv.rfind("SECAM", 0) == 0) return Tv::SECAM;
  return Tv::NTSC;
}

// Luminance in bits 1-3, hue in bits 4-7. NTSC turns the hue around the colour wheel
// in steps of about 26 degrees from gold; PAL alternates two arcs (warm on even hues,
// cool on odd ones) and leaves hues 0, 1, 14 and 15 grey; SECAM ignores the hue and
// gives one of eight fixed colours per luminance.
static uint32_t yiq(double y, double angleDeg, double sat) {
  double angle = angleDeg * 3.14159265358979 / 180.0;
  double i = sat * std::cos(angle), q = sat * std::sin(angle);
  double channels[3] = {y + 0.956 * i + 0.621 * q, y - 0.272 * i - 0.647 * q, y - 1.106 * i + 1.703 * q};
  uint32_t out = 0;
  for (double value : channels) out = out << 8 | (uint32_t)std::lround(std::clamp(value, 0.0, 1.0) * 255);
  return out;
}

uint32_t rgb(Tv tv, int colour) {
  int hue = (colour >> 4) & 15, lum = (colour >> 1) & 7;
  if (tv == Tv::SECAM) {
    static const uint32_t secam[8] = {0x000000, 0x2121FF, 0xF03C79, 0xFF50FF, 0x7FFF00, 0x7FFFFF, 0xFFFF3F, 0xFFFFFF};
    return secam[lum];
  }
  double y = 0.06 + lum / 7.0 * 0.88;
  if (tv == Tv::NTSC) return hue == 0 ? yiq(y, 0, 0) : yiq(y, -40 + (hue - 1) * 25.7, 0.30);
  if (hue < 2 || hue > 13) return yiq(y, 0, 0);
  if (hue % 2 == 0) return yiq(y, -40 + (hue - 2) / 2 * 30.0, 0.28);
  return yiq(y, -95 - (hue - 3) / 2 * 25.0, 0.28);
}

std::string hexColour(int colour) {
  char buffer[8];
  std::snprintf(buffer, sizeof buffer, "$%02X", colour & 0xFE);
  return buffer;
}

// ---------------------------------------------------------------------------
// Sprites

const char* objName(Obj object) {
  static const char* names[kObjects] = {"P0", "P1", "M0", "M1", "BL", "P2", "P3", "P4", "P5", "P6",
                                    "P7", "P8", "P9", "P10", "P11", "P12", "P13", "P14", "P15", "P16"};
  return names[(int)object];
}
bool isPlayer(Obj object) { return object == Obj::P0 || object == Obj::P1 || (int)object >= (int)Obj::P2; }
int playerNumber(Obj object) { return object == Obj::P0 ? 0 : object == Obj::P1 ? 1 : (int)object >= (int)Obj::P2 ? (int)object - (int)Obj::P2 + 2 : -1; }
Obj playerObj(int number) { return number == 0 ? Obj::P0 : number == 1 ? Obj::P1 : (Obj)((int)Obj::P2 + number - 2); }
const char* colourRegister(Obj object) {
  return object == Obj::P0 || object == Obj::M0 ? "COLUP0" : object == Obj::BL ? "COLUPF" : "COLUP1";
}
static int objIndex(Obj object) { return (int)object; }

bool Layer::pixel(int frame, int row, int x) const {
  if (frame < 0 || frame >= (int)bits.size() || row < 0 || row >= (int)bits[frame].size() || x < 0 || x >= width()) return false;
  uint8_t rowValue = bits[frame][row];
  if (!isPlayer(obj)) return rowValue != 0;
  int bitIndex = x / size;
  if (reflect) bitIndex = 7 - bitIndex;
  return (rowValue >> (7 - bitIndex)) & 1;
}

int Layer::colour(int frame, int row) const {
  if (frame < 0 || frame >= (int)colours.size()) return 0x0E;
  for (int i = std::min(row, (int)colours[frame].size() - 1); i >= 0; i--)
    if (colours[frame][i] >= 0) return colours[frame][i];
  return 0x0E;
}

void Layer::resize(int frames, int newHeight) {
  height = std::max(1, newHeight);
  bits.resize(frames);
  colours.resize(frames);
  for (auto& frameRows : bits) frameRows.resize(height, 0);
  for (auto& frameRows : colours) frameRows.resize(height, -1);
}

int Sprite::phases() const {
  int count = 1;
  for (auto& layer : layers) count = std::max(count, layer.phase + 1);
  return count;
}

void Sprite::setFrames(int count) {
  frames = std::max(1, count);
  for (auto& layer : layers) layer.resize(frames, layer.height);
}

void Sprite::insertFrame(int at, int copyOf) {
  at = std::clamp(at, 0, frames);
  for (auto& layer : layers) {
    layer.resize(frames, layer.height);
    std::vector<uint8_t> frameBits(layer.height, 0);
    std::vector<int> frameColours(layer.height, -1);
    if (copyOf >= 0 && copyOf < frames) frameBits = layer.bits[copyOf], frameColours = layer.colours[copyOf];
    layer.bits.insert(layer.bits.begin() + at, frameBits);
    layer.colours.insert(layer.colours.begin() + at, frameColours);
  }
  for (auto& animation : animations)
    for (auto& step : animation.steps)
      if (step.frame >= at) step.frame++;
  frames++;
}

int Animation::length() const {
  int total = 0;
  for (auto& step : steps) total += std::max(1, step.ticks);
  return total;
}

int Sprite::frameAt(const Animation& animation, int tick) const {
  if (animation.steps.empty()) return 0;
  int totalTicks = animation.length();
  tick = animation.loop ? tick % totalTicks : std::min(tick, totalTicks - 1);
  for (auto& step : animation.steps) {
    if (tick < std::max(1, step.ticks)) return std::clamp(step.frame, 0, frames - 1);
    tick -= std::max(1, step.ticks);
  }
  return std::clamp(animation.steps.back().frame, 0, frames - 1);
}

void Sprite::swapFrames(int first, int second) {
  if (first < 0 || second < 0 || first >= frames || second >= frames || first == second) return;
  for (auto& layer : layers) std::swap(layer.bits[first], layer.bits[second]), std::swap(layer.colours[first], layer.colours[second]);
  for (auto& animation : animations)
    for (auto& step : animation.steps) step.frame = step.frame == first ? second : step.frame == second ? first : step.frame;
}

void Sprite::removeFrame(int at) {
  if (frames <= 1 || at < 0 || at >= frames) return;
  for (auto& animation : animations) {
    animation.steps.erase(std::remove_if(animation.steps.begin(), animation.steps.end(), [&](const AnimStep& step) { return step.frame == at; }), animation.steps.end());
    for (auto& step : animation.steps)
      if (step.frame > at) step.frame--;
  }
  for (auto& layer : layers) {
    layer.resize(frames, layer.height);
    layer.bits.erase(layer.bits.begin() + at);
    layer.colours.erase(layer.colours.begin() + at);
  }
  frames--;
}

// ---------------------------------------------------------------------------
// Playfields

void pfBit(int column, int* registerIndex, int* bitIndex) {
  if (column < 4) *registerIndex = 0, *bitIndex = 4 + column;            // PF0: bits 4-7, left to right
  else if (column < 12) *registerIndex = 1, *bitIndex = 7 - (column - 4); // PF1: bits 7-0
  else *registerIndex = 2, *bitIndex = column - 12;                  // PF2: bits 0-7
}

bool Playfield::pixel(int row, int column) const {
  if (row < 0 || row >= (int)rows.size() || column < 0 || column >= 40) return false;
  const PfRow& pfRow = rows[row];
  if (column >= 20 && !pfRow.asymmetric) column = pfRow.mode == PfMode::Mirror ? 39 - column : column - 20;
  return (pfRow.bits >> column) & 1;
}

void Playfield::setAsymmetric(int row, bool on) {
  if (row < 0 || row >= (int)rows.size() || rows[row].asymmetric == on) return;
  PfRow& pfRow = rows[row];
  if (on) {
    uint64_t right = 0;
    for (int column = 20; column < 40; column++)
      if (pixel(row, column)) right |= 1ull << column;
    pfRow.bits = (pfRow.bits & 0xFFFFFull) | right;
  } else {
    pfRow.bits &= 0xFFFFFull;
  }
  pfRow.asymmetric = on;
}

void Playfield::setPixel(int row, int column, bool on) {
  if (row < 0 || row >= (int)rows.size() || column < 0 || column >= 40) return;
  if (column >= 20) setAsymmetric(row, true);
  uint64_t mask = 1ull << column;
  rows[row].bits = on ? rows[row].bits | mask : rows[row].bits & ~mask;
}

int Playfield::pfAt(int row) const {
  for (int i = std::min(row, (int)rows.size() - 1); i >= 0; i--)
    if (rows[i].pf >= 0) return rows[i].pf;
  return pfColour;
}
int Playfield::bkAt(int row) const {
  for (int i = std::min(row, (int)rows.size() - 1); i >= 0; i--)
    if (rows[i].bk >= 0) return rows[i].bk;
  return bkColour;
}

void Playfield::registers(int row, uint8_t left[3], uint8_t right[3]) const {
  for (int i = 0; i < 3; i++) left[i] = right[i] = 0;
  int registerIndex, bitIndex;
  for (int j = 0; j < 20; j++)
    if (pixel(row, j)) pfBit(j, &registerIndex, &bitIndex), left[registerIndex] |= 1 << bitIndex;
  const PfRow& pfRow = rows[row];
  for (int column = 20; column < 40; column++) {
    int sourceColumn = pfRow.mode == PfMode::Mirror ? 39 - column : column - 20;
    if (pixel(row, column)) pfBit(sourceColumn, &registerIndex, &bitIndex), right[registerIndex] |= 1 << bitIndex;
  }
}

// ---------------------------------------------------------------------------
// JSON. Rows are "X" and "." so the file reads (and diffs) as a picture.

static std::string rowString(uint64_t bits, int count, bool msbFirst) {
  std::string text;
  for (int i = 0; i < count; i++) text += ((bits >> (msbFirst ? count - 1 - i : i)) & 1) ? 'X' : '.';
  return text;
}
static uint64_t rowBits(const std::string& text, bool msbFirst) {
  uint64_t value = 0;
  int count = (int)text.size();
  for (int i = 0; i < count; i++)
    if (text[i] == 'X' || text[i] == 'x' || text[i] == '1' || text[i] == '#') value |= 1ull << (msbFirst ? count - 1 - i : i);
  return value;
}

std::string toJson(const Asset& asset) {
  json document;
  document["vcsgfx"] = 1;
  if (!asset.kernel.empty()) document["kernel"] = asset.kernel;
  if (asset.split != Asset::Split::One) document["split"] = asset.split == Asset::Split::Each ? "each" : "group";
  document["sprites"] = json::array();
  for (auto& sprite : asset.sprites) {
    json spriteJson = {{"name", sprite.name}, {"frames", sprite.frames}, {"line_height", sprite.lineHeight}, {"frame_ticks", sprite.frameTicks}};
    if (sprite.background) spriteJson["background"] = sprite.background;
    if (!sprite.group.empty()) spriteJson["group"] = sprite.group;
    spriteJson["layers"] = json::array();
    for (auto& layer : sprite.layers) {
      json layerJson = {{"object", objName(layer.obj)}, {"name", layer.name}, {"dx", layer.dx},     {"dy", layer.dy},
                 {"size", layer.size},           {"reflect", layer.reflect}, {"phase", layer.phase}, {"height", layer.height}};
      layerJson["frames"] = json::array();
      for (int frame = 0; frame < (int)layer.bits.size(); frame++) {
        json rows = json::array();
        for (auto rowValue : layer.bits[frame]) rows.push_back(rowString(rowValue, isPlayer(layer.obj) ? 8 : 1, true));
        layerJson["frames"].push_back({{"rows", rows}, {"colours", frame < (int)layer.colours.size() ? layer.colours[frame] : std::vector<int>{}}});
      }
      spriteJson["layers"].push_back(layerJson);
    }
    if (!sprite.animations.empty()) {
      spriteJson["animations"] = json::array();
      for (auto& animation : sprite.animations) {
        json animationJson = {{"name", animation.name}, {"loop", animation.loop}};
        animationJson["steps"] = json::array();
        for (auto& step : animation.steps) animationJson["steps"].push_back({step.frame, step.ticks});
        if (!animation.basicTimer.empty() || !animation.basicStep.empty()) animationJson["basic_vars"] = {animation.basicTimer, animation.basicStep};
        spriteJson["animations"].push_back(animationJson);
      }
    }
    document["sprites"].push_back(spriteJson);
  }
  document["playfields"] = json::array();
  for (auto& playfield : asset.playfields) {
    json playfieldJson = {{"name", playfield.name}, {"line_height", playfield.lineHeight}, {"pf", playfield.pfColour}, {"bk", playfield.bkColour}};
    if (!playfield.group.empty()) playfieldJson["group"] = playfield.group;
    playfieldJson["rows"] = json::array();
    for (auto& row : playfield.rows) {
      json rowJson = {{"bits", rowString(row.bits, row.asymmetric ? 40 : 20, false)}, {"mode", row.mode == PfMode::Mirror ? "mirror" : "repeat"}};
      if (row.pf >= 0) rowJson["pf"] = row.pf;
      if (row.bk >= 0) rowJson["bk"] = row.bk;
      playfieldJson["rows"].push_back(rowJson);
    }
    document["playfields"].push_back(playfieldJson);
  }
  return document.dump(1, ' ') + "\n";
}

bool fromJson(const std::string& text, Asset* asset, std::string* error) {
  try {
    json document = json::parse(text);
    Asset out;
    out.kernel = document.value("kernel", "");
    std::string split = document.value("split", "");
    out.split = split == "each" ? Asset::Split::Each : split == "group" ? Asset::Split::Group : Asset::Split::One;
    for (auto& spriteJson : document.value("sprites", json::array())) {
      Sprite sprite;
      sprite.name = spriteJson.value("name", sprite.name);
      sprite.group = spriteJson.value("group", "");
      sprite.frames = std::max(1, spriteJson.value("frames", 1));
      sprite.lineHeight = std::clamp(spriteJson.value("line_height", 2), 1, 8);
      sprite.frameTicks = std::clamp(spriteJson.value("frame_ticks", 8), 1, 60);
      sprite.background = spriteJson.value("background", 0) & 0xFE;
      for (auto& layerJson : spriteJson.value("layers", json::array())) {
        Layer layer;
        std::string objectName = layerJson.value("object", "P0");
        for (int objectIndex = 0; objectIndex < kObjects; objectIndex++)
          if (objectName == objName((Obj)objectIndex)) layer.obj = (Obj)objectIndex;
        layer.name = layerJson.value("name", "");
        layer.dx = layerJson.value("dx", 0);
        layer.dy = layerJson.value("dy", 0);
        layer.size = layerJson.value("size", 1);
        layer.reflect = layerJson.value("reflect", false);
        layer.phase = std::clamp(layerJson.value("phase", 0), 0, 3);
        layer.resize(sprite.frames, std::clamp(layerJson.value("height", 8), 1, 255));
        int frameIndex = 0;
        for (auto& frameJson : layerJson.value("frames", json::array())) {
          if (frameIndex >= sprite.frames) break;
          int rowIndex = 0;
          for (auto& row : frameJson.value("rows", json::array()))
            if (rowIndex < layer.height) layer.bits[frameIndex][rowIndex++] = (uint8_t)rowBits(row.get<std::string>(), true);
          rowIndex = 0;
          for (auto& colour : frameJson.value("colours", json::array()))
            if (rowIndex < layer.height) layer.colours[frameIndex][rowIndex++] = colour.get<int>();
          frameIndex++;
        }
        sprite.layers.push_back(layer);
      }
      for (auto& animationJson : spriteJson.value("animations", json::array())) {
        Animation animation;
        animation.name = animationJson.value("name", animation.name);
        animation.loop = animationJson.value("loop", true);
        for (auto& stepJson : animationJson.value("steps", json::array()))
          if (stepJson.is_array() && stepJson.size() == 2)
            animation.steps.push_back({std::clamp(stepJson[0].get<int>(), 0, sprite.frames - 1), std::clamp(stepJson[1].get<int>(), 1, 255)});
        auto vars = animationJson.value("basic_vars", json::array());
        if (vars.size() == 2) animation.basicTimer = vars[0].get<std::string>(), animation.basicStep = vars[1].get<std::string>();
        sprite.animations.push_back(animation);
      }
      out.sprites.push_back(sprite);
    }
    for (auto& playfieldJson : document.value("playfields", json::array())) {
      Playfield playfield;
      playfield.name = playfieldJson.value("name", playfield.name);
      playfield.group = playfieldJson.value("group", "");
      playfield.lineHeight = std::clamp(playfieldJson.value("line_height", 8), 1, 64);
      playfield.pfColour = playfieldJson.value("pf", playfield.pfColour);
      playfield.bkColour = playfieldJson.value("bk", playfield.bkColour);
      playfield.rows.clear();
      for (auto& rowJson : playfieldJson.value("rows", json::array())) {
        PfRow pfRow;
        std::string bitsText = rowJson.value("bits", "");
        pfRow.bits = rowBits(bitsText, false);
        pfRow.asymmetric = bitsText.size() > 20;
        pfRow.mode = rowJson.value("mode", "mirror") == "repeat" ? PfMode::Repeat : PfMode::Mirror;
        pfRow.pf = rowJson.value("pf", -1);
        pfRow.bk = rowJson.value("bk", -1);
        playfield.rows.push_back(pfRow);
      }
      if (playfield.rows.empty()) playfield.rows.resize(1);
      out.playfields.push_back(playfield);
    }
    *asset = out;
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

// ---------------------------------------------------------------------------
// Kernels. What each batari Basic kernel shows was checked with test programs built
// with batari Basic 1.9 and run in Stella 7.0 (row order, columns, colours), and in the
// kernels' sources (includes/*.asm, *.h).

const char* kernelId(Kernel kernel) {
  static const char* ids[] = {"dasm", "standard", "multisprite", "dpc+", "pxe"};
  return ids[(int)kernel];
}
const char* kernelName(Kernel kernel) {
  switch (kernel) {
    case Kernel::Dasm: return T("assembly (a kernel of one's own)");
    case Kernel::Standard: return T("batari Basic, standard kernel");
    case Kernel::Multisprite: return T("batari Basic, multisprite kernel");
    case Kernel::DPCPlus: return T("batari Basic, DPC+ kernel");
    case Kernel::PXE: return T("batari Basic, PXE kernel");
  }
  return "";
}
bool kernelFromId(const std::string& id, Kernel* kernel) {
  for (int i = 0; i < 5; i++)
    if (id == kernelId((Kernel)i)) return *kernel = (Kernel)i, true;
  return false;
}

std::string KernelOptions::text() const {
  std::string result;
  auto add = [&](bool on, const char* name) {
    if (on) result += (result.empty() ? "" : " ") + std::string(name);
  };
  add(player1colors, "player1colors");
  add(playercolors, "playercolors");
  add(pfcolors, "pfcolors");
  add(pfheights, "pfheights");
  add(background, "background");
  add(noBlankLines, "no_blank_lines");
  add(readpaddle, "readpaddle");
  if (pfres) result += (result.empty() ? "" : ", ") + std::string("pfres ") + std::to_string(pfres);
  return result;
}

int Profile::players() const {
  switch (kernel) {
    case Kernel::Multisprite: return 6;
    case Kernel::DPCPlus: return 10;
    case Kernel::PXE: return 17;
    default: return 2;
  }
}

std::string Profile::missing(Obj object) const {
  int number = playerNumber(object);
  if (number >= 0) return number < players() ? "" : F("{0} draws {1} players (P0-P{2}).", {kernelName(kernel), std::to_string(players()), std::to_string(players() - 1)});
  if (kernel != Kernel::Standard) return "";
  // the standard kernel spends the missiles' time on other things
  if (object == Obj::M1 && (options.player1colors || options.playercolors)) return T("The kernel option player1colors (or playercolors) uses missile 1's time.");
  if (object == Obj::M0 && options.playercolors) return T("The kernel option playercolors uses both missiles' time.");
  if (object == Obj::M0 && (options.noBlankLines || options.readpaddle)) return T("The kernel options no_blank_lines and readpaddle use missile 0's time.");
  return "";
}
bool Profile::has(Obj object) const { return missing(object).empty(); }

bool Profile::rowColours(Obj object) const {
  switch (kernel) {
    case Kernel::Dasm: return true;
    case Kernel::Standard:
      if (object == Obj::P0) return options.playercolors;
      if (object == Obj::P1) return options.player1colors || options.playercolors;
      return false;
    case Kernel::Multisprite: return false;
    default: return isPlayer(object);  // DPC+ and PXE: playerNcolor:
  }
}
bool Profile::shapedMissiles() const { return kernel == Kernel::Dasm; }
bool Profile::topFirst() const { return kernel == Kernel::Dasm || kernel == Kernel::DPCPlus || kernel == Kernel::PXE; }
int Profile::pfFirst() const { return kernel == Kernel::Dasm ? 0 : 4; }
int Profile::pfColumns() const { return kernel == Kernel::Dasm ? 40 : kernel == Kernel::Multisprite ? 16 : 32; }
bool Profile::pfMirrorOnly() const { return kernel == Kernel::Multisprite; }
bool Profile::pfAsymmetric() const { return kernel != Kernel::Multisprite; }
bool Profile::pfRowModes() const { return kernel == Kernel::Dasm; }
bool Profile::pfRowColours() const {
  if (kernel == Kernel::Standard) return options.pfcolors && !options.background;
  return kernel != Kernel::Multisprite;
}
bool Profile::bkRowColours() const {
  if (kernel == Kernel::Standard) return options.pfcolors && options.background;  // the same table, to COLUBK
  return kernel == Kernel::Dasm || kernel == Kernel::DPCPlus || kernel == Kernel::PXE;
}
int Profile::pfRowsDefault() const {
  if (kernel == Kernel::Standard) return options.pfres ? options.pfres : 11;
  if (kernel == Kernel::Multisprite) return 11;
  return 0;
}
int Profile::pfRowsMax() const {
  switch (kernel) {
    case Kernel::Standard: return 32;  // pfres above 12 needs cartridge RAM
    case Kernel::Multisprite: return 44;
    case Kernel::DPCPlus:
    case Kernel::PXE: return 176;
    default: return 192;
  }
}
std::string Profile::describe() const {
  if (kernel == Kernel::Dasm && usesC) return T("C (a kernel of one's own)");
  std::string optionsText = kernel == Kernel::Standard ? options.text() : "";
  return optionsText.empty() ? kernelName(kernel) : std::string(kernelName(kernel)) + " (" + optionsText + ")";
}

Profile profileFromBasic(const std::string& source) {
  Profile profile;
  profile.kernel = Kernel::Standard;
  std::istringstream sourceStream(source);
  for (std::string line; std::getline(sourceStream, line);) {
    std::string lowered;
    for (char character : line.substr(0, line.find(';'))) lowered += (char)std::tolower((unsigned char)character);
    std::istringstream words(lowered);
    std::string keyword, argument;
    words >> keyword >> argument;
    if (keyword == "rem") continue;
    if (keyword == "set" && argument == "kernel") {
      std::string kernelWord;
      words >> kernelWord;
      if (kernelWord == "multisprite") profile.kernel = Kernel::Multisprite;
      else if (kernelWord == "dpc+") profile.kernel = Kernel::DPCPlus;
      else if (kernelWord == "pxe") profile.kernel = Kernel::PXE;
    } else if (keyword == "set" && argument == "kernel_options") {
      for (std::string option; words >> option;) {
        profile.options.player1colors |= option == "player1colors";
        profile.options.playercolors |= option == "playercolors";
        profile.options.pfcolors |= option == "pfcolors";
        profile.options.pfheights |= option == "pfheights";
        profile.options.background |= option == "background" || option == "backgroundchange";
        profile.options.noBlankLines |= option == "no_blank_lines";
        profile.options.readpaddle |= option == "readpaddle";
      }
    } else if (keyword == "const" && argument.rfind("pfres", 0) == 0) {
      size_t eq = lowered.find('=');
      if (eq != std::string::npos) profile.options.pfres = std::atoi(lowered.c_str() + eq + 1);
    }
  }
  return profile;
}

// ---------------------------------------------------------------------------
// Warnings

static std::string rowList(const std::vector<int>& rows) {
  std::string text;
  for (size_t i = 0; i < rows.size() && i < 8; i++) text += (i ? ", " : "") + std::to_string(rows[i]);
  if (rows.size() > 8) text += ", …";
  return text;
}

static bool rowHasPixels(const Layer& layer, int frame, int row) {
  return frame < (int)layer.bits.size() && row >= 0 && row < (int)layer.bits[frame].size() && layer.bits[frame][row] != 0;
}

static bool coloursVary(const Layer& layer) {
  for (int frame = 0; frame < (int)layer.bits.size(); frame++)
    for (int row = 1; row < layer.height; row++)
      if (layer.colour(frame, row) != layer.colour(frame, 0)) return true;
  return false;
}

std::vector<Issue> check(const Sprite& sprite, const Profile& profile) {
  std::vector<Issue> out;
  int phases = sprite.phases();
  // objects the kernel does not draw
  std::set<int> told;
  for (auto& layer : sprite.layers)
    if (!profile.has(layer.obj) && told.insert((int)layer.obj).second)
      out.push_back({Issue::Error, F("{0} is not available: {1}", {objName(layer.obj), profile.missing(layer.obj)})});
  // one object per phase
  for (int phase = 0; phase < phases; phase++) {
    int used[kObjects] = {};
    for (auto& layer : sprite.layers)
      if (layer.phase == phase) used[objIndex(layer.obj)]++;
    for (int objectIndex = 0; objectIndex < kObjects; objectIndex++)
      if (used[objectIndex] > 1)
        out.push_back({Issue::Error, F("Phase {0} uses {1} in {2} layers: the TIA has only one {1}. Move one of them to another phase.",
                                       {std::to_string(phase + 1), objName((Obj)objectIndex), std::to_string(used[objectIndex])})});
  }
  if (phases > 1) {
    std::vector<int> empty;
    for (int phase = 0; phase < phases; phase++)
      if (std::none_of(sprite.layers.begin(), sprite.layers.end(), [&](const Layer& layer) { return layer.phase == phase; })) empty.push_back(phase + 1);
    out.push_back({Issue::Warning, F("{0} flicker phases: each layer shows one frame in {0} ({1} Hz on NTSC, {2} Hz on PAL) and flickers.",
                                     {std::to_string(phases), std::to_string(60 / phases), std::to_string(50 / phases)})});
    if (!empty.empty()) out.push_back({Issue::Warning, F("Phases without layers: {0}. The other layers flicker more than they need to.", {rowList(empty)})});
  }
  // animations: names become labels
  std::set<std::string> animNames;
  for (auto& animation : sprite.animations) {
    if (animation.steps.empty()) out.push_back({Issue::Warning, F("The animation {0} has no steps.", {animation.name})});
    if (!animNames.insert(identifier(animation.name)).second)
      out.push_back({Issue::Error, F("Two animations are called {0}: their labels would clash.", {animation.name})});
  }
  // the kernels with more players draw them all with the TIA's player 1
  if (profile.players() > 2) {
    bool shared = false;
    for (size_t i = 0; i < sprite.layers.size() && !shared; i++)
      for (size_t j = i + 1; j < sprite.layers.size() && !shared; j++) {
        const Layer &earlier = sprite.layers[i], &later = sprite.layers[j];
        shared = earlier.phase == later.phase && playerNumber(earlier.obj) >= 1 && playerNumber(later.obj) >= 1 && earlier.obj != later.obj &&
                 earlier.dy < later.dy + later.height && later.dy < earlier.dy + earlier.height;
      }
    if (shared)
      out.push_back({Issue::Warning, F("P1 to P{0} are all drawn with the TIA's player 1: where two of them share lines the kernel shows them on alternate frames (they flicker). Put them in different rows, or combine P0 with one of them.",
                                       {std::to_string(profile.players() - 1)})});
  }
  // a missile shares its player's colour register
  for (auto& missile : sprite.layers) {
    if (missile.obj != Obj::M0 && missile.obj != Obj::M1) continue;
    Obj playerObject = missile.obj == Obj::M0 ? Obj::P0 : Obj::P1;
    for (auto& playerLayer : sprite.layers) {
      if (playerLayer.obj != playerObject || playerLayer.phase != missile.phase) continue;
      std::vector<int> bad;
      for (int frame = 0; frame < sprite.frames; frame++)
        for (int row = 0; row < missile.height; row++) {
          int playerRow = row + missile.dy - playerLayer.dy;
          if (rowHasPixels(missile, frame, row) && playerRow >= 0 && playerRow < playerLayer.height && missile.colour(frame, row) != playerLayer.colour(frame, playerRow) &&
              std::find(bad.begin(), bad.end(), row + missile.dy + 1) == bad.end())
            bad.push_back(row + missile.dy + 1);
        }
      if (!bad.empty())
        out.push_back({Issue::Warning, F("{0} takes the colour of {1} ({2}). On rows {3} they differ: {0} will show {1}'s.",
                                         {objName(missile.obj), objName(playerObject), colourRegister(playerObject), rowList(bad)})});
    }
  }
  for (auto& ball : sprite.layers)
    if (ball.obj == Obj::BL)
      out.push_back({Issue::Info, coloursVary(ball) ? T("The ball's colour is the playfield's (COLUPF): changing it per row recolours the playfield on those lines.")
                                                 : T("The ball's colour is the playfield's (COLUPF).")});
  // overlaps: the TIA's priority decides, not the layer order
  for (int phase = 0; phase < phases; phase++) {
    bool overlap = false;
    for (size_t i = 0; i < sprite.layers.size() && !overlap; i++)
      for (size_t j = i + 1; j < sprite.layers.size() && !overlap; j++) {
        const Layer &earlier = sprite.layers[i], &later = sprite.layers[j];
        if (earlier.phase != phase || later.phase != phase || earlier.obj == later.obj) continue;
        for (int frame = 0; frame < sprite.frames && !overlap; frame++)
          for (int row = 0; row < earlier.height && !overlap; row++)
            for (int x = 0; x < earlier.width() && !overlap; x++)
              overlap = earlier.pixel(frame, row, x) && later.pixel(frame, row + earlier.dy - later.dy, x + earlier.dx - later.dx);
      }
    if (overlap) {
      out.push_back({Issue::Info, T("Some layers overlap: the TIA draws P0 and M0 over P1 and M1, and those over the ball and playfield (the preview does the same).")});
      break;
    }
  }
  if (!profile.basic()) {
    // what the kernel has to write per line
    for (int phase = 0; phase < phases; phase++) {
      int writes = 0;
      std::set<std::string> registers;
      for (auto& layer : sprite.layers) {
        if (layer.phase != phase) continue;
        writes++;
        if (coloursVary(layer) && registers.insert(colourRegister(layer.obj)).second) writes++;
      }
      if (writes > 4)
        out.push_back({Issue::Warning, F("Phase {0} changes {1} registers on the same lines (graphics and colours): about {2} cycles of loads and stores of the 76 a line has. It may need a two-line kernel.",
                                         {std::to_string(phase + 1), std::to_string(writes), std::to_string(writes * 8)})});
    }
    return out;
  }
  for (auto& layer : sprite.layers) {
    if (isPlayer(layer.obj) || !profile.has(layer.obj)) continue;
    if (profile.kernel == Kernel::Multisprite && layer.obj != Obj::BL) {
      out.push_back({Issue::Warning, F("The multisprite kernel draws {0} one line high: only its position is kept.", {objName(layer.obj)})});
      continue;
    }
    // batari Basic's missiles and ball are a block: height and width
    bool block = true;
    for (int frame = 0; frame < sprite.frames && block; frame++) {
      int first = -1, last = -1;
      for (int row = 0; row < layer.height; row++)
        if (rowHasPixels(layer, frame, row)) {
          if (first < 0) first = row;
          if (last >= 0 && last != row - 1) block = false;
          last = row;
        }
      if (frame > 0 && layer.bits[frame] != layer.bits[0]) block = false;
    }
    if (!block)
      out.push_back({Issue::Warning, F("batari Basic draws {0} as a solid block (a height and a width): its shape here is not kept, only its height.", {objName(layer.obj)})});
  }
  if (profile.kernel == Kernel::Standard && profile.options.playercolors && !profile.options.player1colors)
    out.push_back({Issue::Warning, T("The kernel option playercolors works together with player1colors: `set kernel_options player1colors playercolors`.")});
  if (phases > 1)
    out.push_back({Issue::Warning, T("batari Basic does not alternate the phases by itself: each one is exported on its own, and the program has to switch them every frame.")});
  for (auto& layer : sprite.layers) {
    if (!isPlayer(layer.obj) || !profile.has(layer.obj) || !coloursVary(layer) || profile.rowColours(layer.obj)) continue;
    if (profile.kernel == Kernel::Standard)
      out.push_back({Issue::Warning, F("{0} changes colour per row, and the program does not set `set kernel_options {1}`: only the first row's colour is kept.",
                                       {objName(layer.obj), layer.obj == Obj::P0 ? "playercolors" : "player1colors"})});
    else
      out.push_back({Issue::Warning, F("{0} changes colour per row, but this kernel gives each player one colour: only the first row's is kept.", {objName(layer.obj)})});
  }
  if (sprite.layers.size() > 1) out.push_back({Issue::Info, T("The layers' positions are exported as comments: the program places each object relative to the first one.")});
  for (auto& animation : sprite.animations)
    if (animation.basicTimer.empty() || animation.basicStep.empty())
      out.push_back({Issue::Warning, F("The animation {0} needs two batari Basic variables (its counter and its step): choose them in the animation.", {animation.name})});
  return out;
}

std::vector<Issue> check(const Playfield& playfield, const Profile& profile) {
  std::vector<Issue> out;
  std::vector<int> asym, modeChange, pfChange, bkChange;
  for (int row = 0; row < (int)playfield.rows.size(); row++) {
    if (playfield.rows[row].asymmetric) asym.push_back(row + 1);
    if (row > 0 && playfield.rows[row].mode != playfield.rows[row - 1].mode) modeChange.push_back(row + 1);
    if (row > 0 && playfield.pfAt(row) != playfield.pfAt(row - 1)) pfChange.push_back(row + 1);
    if (row > 0 && playfield.bkAt(row) != playfield.bkAt(row - 1)) bkChange.push_back(row + 1);
  }
  if (!profile.basic()) {
    if (!asym.empty()) {
      out.push_back({Issue::Info, F("Asymmetric rows: {0}. On every line of them PF0, PF1 and PF2 are written again in the middle of the line, each one in its own window of cycles.", {rowList(asym)})});
      bool mirrorAsym = std::any_of(playfield.rows.begin(), playfield.rows.end(), [](const PfRow& pfRow) { return pfRow.asymmetric && pfRow.mode == PfMode::Mirror; });
      if (mirrorAsym)
        out.push_back({Issue::Warning, T("An asymmetric row in mirror mode shows PF2 twice back to back in the middle of the line: its write has almost no margin. Repeat mode is easier.")});
    }
    if (!modeChange.empty())
      out.push_back({Issue::Info, F("Repeat/mirror changes on rows {0}: the kernel writes CTRLPF there, and CTRLPF also holds the ball's size and the playfield priority.", {rowList(modeChange)})});
    int writes = 3 + (asym.empty() ? 0 : 3) + (pfChange.empty() ? 0 : 1) + (bkChange.empty() ? 0 : 1);
    if (writes >= 7)
      out.push_back({Issue::Warning, F("Some lines change {0} registers (playfield and colours): together with sprites it may not fit in one line.", {std::to_string(writes)})});
    return out;
  }
  // columns the kernel does not show
  int first = profile.pfFirst(), last = profile.pfMirrorOnly() ? 19 : 39 - first;
  std::vector<int> lost, notMirror;
  for (int row = 0; row < (int)playfield.rows.size(); row++) {
    bool rowLost = false, rowNotMirrored = false;
    for (int column = 0; column < 40; column++) {
      bool shown = column >= first && column <= last;
      if (profile.pfMirrorOnly() && column >= 20) shown = 39 - column >= first, rowNotMirrored |= playfield.pixel(row, column) != playfield.pixel(row, 39 - column);
      rowLost |= !shown && playfield.pixel(row, column);
    }
    if (rowLost) lost.push_back(row + 1);
    if (rowNotMirrored) notMirror.push_back(row + 1);
  }
  if (!lost.empty())
    out.push_back({Issue::Warning, F("This kernel shows columns {0} to {1} only: what is outside them is lost (rows {2}).",
                                     {std::to_string(first + 1), std::to_string(40 - first), rowList(lost)})});
  if (!notMirror.empty())
    out.push_back({Issue::Warning, F("The multisprite kernel's playfield is mirrored (the right half is the left one reversed); these rows are not: {0}.", {rowList(notMirror)})});
  int rows = (int)playfield.rows.size();
  if (rows > profile.pfRowsMax())
    out.push_back({Issue::Error, F("{0} rows: this kernel shows {1} at most.", {std::to_string(rows), std::to_string(profile.pfRowsMax())})});
  else if (profile.pfRowsDefault() && rows != profile.pfRowsDefault()) {
    if (profile.kernel == Kernel::Standard)
      out.push_back({Issue::Warning, F("{0} rows, and the program shows {1}: it needs `const pfres = {0}` (only some values are allowed, and more rows take more RAM).",
                                       {std::to_string(rows), std::to_string(profile.pfRowsDefault())})});
    else
      out.push_back({Issue::Warning, F("{0} rows: this kernel shows {1}.", {std::to_string(rows), std::to_string(profile.pfRowsDefault())})});
  }
  out.push_back({Issue::Info, T("batari Basic sets the height of the rows itself; the line height here is only for the preview.")});
  bool pfVary = !pfChange.empty(), bkVary = !bkChange.empty();
  if (pfVary && !profile.pfRowColours()) {
    if (profile.kernel != Kernel::Standard) out.push_back({Issue::Warning, T("This kernel gives the playfield one colour: only the first row's is kept.")});
    else if (profile.options.background) out.push_back({Issue::Warning, T("With the kernel option background the table of colours goes to the background: the playfield keeps one colour.")});
    else out.push_back({Issue::Warning, T("The rows have their own colours, and the program does not set `set kernel_options pfcolors`: only the first row's is kept.")});
  }
  if (bkVary && !profile.bkRowColours()) {
    if (profile.kernel == Kernel::Standard)
      out.push_back({Issue::Warning, profile.options.pfcolors ? T("The background changes per row: the kernel options would have to be pfcolors and background, and then the playfield keeps one colour.")
                                                        : T("The background changes per row: it needs `set kernel_options pfcolors background` (and then the playfield keeps one colour).")});
    else out.push_back({Issue::Warning, T("This kernel does not change the background per row: only the first row's is kept.")});
  }
  return out;
}

// ---------------------------------------------------------------------------
// Export

std::string identifier(const std::string& name) {
  std::string result;
  for (char character : name) result += std::isalnum((unsigned char)character) ? character : '_';
  if (result.empty() || std::isdigit((unsigned char)result[0])) result = "g" + result;
  return result;
}

// "P0", "P0_helmet", "P0b" (a second P0 layer, in another phase)
static std::string layerId(const Layer& layer, const std::vector<Layer>& all) {
  int same = 0, mine = 0;
  for (auto& other : all)
    if (other.obj == layer.obj) {
      if (&other == &layer) mine = same;
      same++;
    }
  std::string id = objName(layer.obj);
  if (same > 1 && mine > 0) id += (char)('a' + std::min(mine, 25));
  if (!layer.name.empty()) id += "_" + identifier(layer.name);
  return id;
}

static std::string bin8(int value) {
  std::string result = "%";
  for (int bit = 7; bit >= 0; bit--) result += (value >> bit) & 1 ? '1' : '0';
  return result;
}

static std::string dasmSpriteText(const Sprite& sprite, const ExportOptions& options) {
  std::ostringstream out;
  std::string spriteName = identifier(sprite.name);
  out << "; " << sprite.name << ": " << sprite.layers.size() << " layer(s), " << sprite.frames << " frame(s), " << sprite.phases()
      << " phase(s). Rows: " << (options.upsideDown ? "bottom first" : "top first") << ".\n";
  for (auto& layer : sprite.layers) {
    std::string id = spriteName + "_" + layerId(layer, sprite.layers);
    out << "\n; " << objName(layer.obj) << (layer.name.empty() ? "" : " (" + layer.name + ")") << ", phase " << layer.phase + 1 << ", "
        << colourRegister(layer.obj) << "\n";
    out << id << "_DX = " << layer.dx << "\n" << id << "_DY = " << layer.dy << "\n" << id << "_HEIGHT = " << layer.height << "\n";
    out << id << "_PHASE = " << layer.phase << "\n";
    if (isPlayer(layer.obj)) {
      out << id << "_NUSIZ = " << (layer.size == 4 ? "$07" : layer.size == 2 ? "$05" : "$00") << "\n";
      if (layer.reflect) out << id << "_REFP = $08\n";
    } else {
      int bits = layer.size >= 8 ? 3 : layer.size >= 4 ? 2 : layer.size >= 2 ? 1 : 0;
      out << id << "_SIZE = $" << bits << "0" << (layer.obj == Obj::BL ? "  ; CTRLPF bits 4-5\n" : "  ; NUSIZ bits 4-5\n");
    }
    for (int frame = 0; frame < sprite.frames; frame++) {
      out << id << "_Frame" << frame << "\n";
      for (int i = 0; i < layer.height; i++) {
        int row = options.upsideDown ? layer.height - 1 - i : i;
        int value = frame < (int)layer.bits.size() ? layer.bits[frame][row] : 0;
        if (isPlayer(layer.obj)) out << "        .byte #" << bin8(value) << ";" << hexColour(layer.colour(frame, row)) << "\n";
        else out << "        .byte #" << bin8(value ? 2 : 0) << "\n";  // ENAMx/ENABL: bit 1
      }
    }
    if (layer.obj != Obj::M0 && layer.obj != Obj::M1)
      for (int frame = 0; frame < sprite.frames; frame++) {
        out << id << "_Colour" << frame << "\n";
        for (int i = 0; i < layer.height; i++) out << "        .byte #" << hexColour(layer.colour(frame, options.upsideDown ? layer.height - 1 - i : i)) << "\n";
      }
    if (sprite.frames > 1) {
      // the frames by number: lda Ptr,x
      auto pointers = [&](const char* table, const char* label) {
        for (const char* half : {"Lo", "Hi"}) {
          out << id << "_" << table << "Ptr" << half << "\n        .byte ";
          for (int frame = 0; frame < sprite.frames; frame++) out << (frame ? "," : "") << (half[0] == 'L' ? "<" : ">") << id << "_" << label << frame;
          out << "\n";
        }
      };
      pointers("Frame", "Frame");
      if (layer.obj != Obj::M0 && layer.obj != Obj::M1) pointers("Colour", "Colour");
    }
  }
  out << "\n" << spriteName << "_FRAMES = " << sprite.frames << "\n";
  for (auto& animation : sprite.animations) {
    // steps: the frame and how many video frames it stays
    std::string prefix = spriteName + "_" + identifier(animation.name);
    out << "\n; animation " << animation.name << ": " << animation.steps.size() << " steps, " << animation.length() << " video frames"
        << (animation.loop ? ", loops" : ", stops on the last step") << "\n";
    out << prefix << "_STEPS = " << animation.steps.size() << "\n" << prefix << "_LOOP = " << (animation.loop ? 1 : 0) << "\n";
    out << prefix << "_FRAME\n        .byte ";
    for (size_t i = 0; i < animation.steps.size(); i++) out << (i ? "," : "") << animation.steps[i].frame;
    out << "\n" << prefix << "_TICKS\n        .byte ";
    for (size_t i = 0; i < animation.steps.size(); i++) out << (i ? "," : "") << animation.steps[i].ticks;
    out << "\n";
  }
  return out.str();
}

std::string dasmInclude(const Asset& asset, const std::string& sourceName) {
  std::string out = "; Generated by VCSIDE from " + sourceName + ": edit that file, not this one.\n";
  for (auto& sprite : asset.sprites) out += "\n" + exportDasm(sprite);
  for (auto& playfield : asset.playfields) out += "\n" + exportDasm(playfield);
  return out;
}

static std::string dasmPlayfieldText(const Playfield& playfield, const ExportOptions& options) {
  std::ostringstream out;
  std::string playfieldName = identifier(playfield.name);
  int rows = (int)playfield.rows.size();
  bool anyAsym = std::any_of(playfield.rows.begin(), playfield.rows.end(), [](const PfRow& row) { return row.asymmetric; });
  bool modes = std::any_of(playfield.rows.begin(), playfield.rows.end(), [&](const PfRow& row) { return row.mode != playfield.rows[0].mode; });
  bool pfVary = false, bkVary = false;
  for (int row = 1; row < rows; row++) pfVary |= playfield.pfAt(row) != playfield.pfAt(0), bkVary |= playfield.bkAt(row) != playfield.bkAt(0);
  out << "; " << playfield.name << ": " << rows << " rows of " << playfield.lineHeight << " lines"
      << (anyAsym ? ", asymmetric rows" : "") << ". Rows: " << (options.upsideDown ? "bottom first" : "top first") << ".\n";
  out << playfieldName << "_ROWS = " << rows << "\n" << playfieldName << "_LINES = " << playfield.lineHeight << "\n";
  out << playfieldName << "_CTRLPF = " << (playfield.rows[0].mode == PfMode::Mirror ? "$01" : "$00") << "  ; first row\n";
  out << playfieldName << "_COLUPF = " << hexColour(playfield.pfAt(0)) << "\n" << playfieldName << "_COLUBK = " << hexColour(playfield.bkAt(0)) << "\n";
  auto table = [&](const std::string& label, auto value, bool picture) {
    out << playfieldName << "_" << label << "\n";
    for (int i = 0; i < rows; i++) {
      int row = options.upsideDown ? rows - 1 - i : i;
      out << "        .byte " << value(row);
      if (picture) {
        out << "  ; |";
        for (int column = 0; column < 40; column++) out << (playfield.pixel(row, column) ? 'X' : '.');
        out << "|";
      }
      out << "\n";
    }
  };
  const char* registerNames[3] = {"PF0", "PF1", "PF2"};
  for (int i = 0; i < 3; i++)
    table(registerNames[i], [&](int row) { uint8_t leftRegisters[3], rightRegisters[3]; playfield.registers(row, leftRegisters, rightRegisters); return bin8(leftRegisters[i]); }, i == 0);
  if (anyAsym)
    for (int i = 0; i < 3; i++)
      table(std::string(registerNames[i]) + "R", [&](int row) { uint8_t leftRegisters[3], rightRegisters[3]; playfield.registers(row, leftRegisters, rightRegisters); return bin8(rightRegisters[i]); }, false);
  if (modes) table("CTRLPF_ROWS", [&](int row) { return std::string(playfield.rows[row].mode == PfMode::Mirror ? "$01" : "$00"); }, false);
  if (pfVary) table("COLUPF_ROWS", [&](int row) { return hexColour(playfield.pfAt(row)); }, false);
  if (bkVary) table("COLUBK_ROWS", [&](int row) { return hexColour(playfield.bkAt(row)); }, false);
  return out.str();
}

// ---------------------------------------------------------------------------
// C

static std::string upper(std::string text) {
  for (auto& character : text) character = (char)std::toupper((unsigned char)character);
  return text;
}
static std::string hex2(int value) {
  static const char* digits = "0123456789ABCDEF";
  return std::string("0x") + digits[(value >> 4) & 15] + digits[value & 15];
}
static std::string picture8(int value) {
  std::string result;
  for (int bit = 7; bit >= 0; bit--) result += (value >> bit) & 1 ? 'X' : '.';
  return result;
}

static std::string cSpriteText(const Sprite& sprite) {
  std::ostringstream out;
  std::string spriteName = identifier(sprite.name), upperName = upper(spriteName);
  out << "/* " << sprite.name << ": " << sprite.layers.size() << " layer(s), " << sprite.frames << " frame(s), " << sprite.phases()
      << " phase(s). Rows: top first. */\n";
  out << "#define " << upperName << "_FRAMES " << sprite.frames << "\n";
  for (auto& layer : sprite.layers) {
    std::string id = spriteName + "_" + layerId(layer, sprite.layers), upperId = upper(id);
    out << "\n/* " << objName(layer.obj) << (layer.name.empty() ? "" : " (" + layer.name + ")") << ", phase " << layer.phase + 1 << ", "
        << colourRegister(layer.obj) << " */\n";
    out << "#define " << upperId << "_DX " << layer.dx << "\n#define " << upperId << "_DY " << layer.dy << "\n#define " << upperId << "_HEIGHT "
        << layer.height << "\n#define " << upperId << "_PHASE " << layer.phase << "\n";
    if (isPlayer(layer.obj)) {
      out << "#define " << upperId << "_NUSIZ " << (layer.size == 4 ? "0x07" : layer.size == 2 ? "0x05" : "0x00") << "\n";
      out << "#define " << upperId << "_REFP " << (layer.reflect ? "0x08" : "0x00") << "\n";
    } else {
      int bits = layer.size >= 8 ? 3 : layer.size >= 4 ? 2 : layer.size >= 2 ? 1 : 0;
      out << "#define " << upperId << "_SIZE 0x" << bits << "0" << (layer.obj == Obj::BL ? "  /* CTRLPF bits 4-5 */\n" : "  /* NUSIZ bits 4-5 */\n");
    }
    // [frame][row]: GRPx for players, ENAMx/ENABL (bit 1) for missiles and ball
    out << "static const uint8_t " << id << "_gfx[" << upperName << "_FRAMES][" << layer.height << "] = {\n";
    for (int frame = 0; frame < sprite.frames; frame++) {
      out << "    { /* frame " << frame << " */\n";
      for (int row = 0; row < layer.height; row++) {
        int value = frame < (int)layer.bits.size() ? layer.bits[frame][row] : 0;
        if (isPlayer(layer.obj)) out << "        " << hex2(value) << ",  /* " << picture8(value) << " " << hexColour(layer.colour(frame, row)) << " */\n";
        else out << "        " << (value ? "0x02" : "0x00") << ",\n";
      }
      out << "    },\n";
    }
    out << "};\n";
    if (layer.obj != Obj::M0 && layer.obj != Obj::M1) {
      out << "static const uint8_t " << id << "_colour[" << upperName << "_FRAMES][" << layer.height << "] = {\n";
      for (int frame = 0; frame < sprite.frames; frame++) {
        out << "    {";
        for (int row = 0; row < layer.height; row++) out << (row ? ", " : "") << hex2(layer.colour(frame, row));
        out << "},\n";
      }
      out << "};\n";
    }
  }
  for (auto& animation : sprite.animations) {
    std::string prefix = spriteName + "_" + identifier(animation.name);
    out << "\n/* animation " << animation.name << ": " << animation.steps.size() << " steps, " << animation.length() << " video frames"
        << (animation.loop ? ", loops" : ", stops on the last step") << " */\n";
    out << "static const vcsgfx_step " << prefix << "_steps[] = {";
    for (size_t i = 0; i < animation.steps.size(); i++) out << (i ? ", " : "") << "{" << animation.steps[i].frame << ", " << std::clamp(animation.steps[i].ticks, 1, 255) << "}";
    if (animation.steps.empty()) out << "{0, 0}";
    out << "};\n";
    out << "static const vcsgfx_animation " << prefix << " = {" << prefix << "_steps, " << animation.steps.size() << ", " << (animation.loop ? 1 : 0)
        << "};\n";
  }
  return out.str();
}

static std::string cPlayfieldText(const Playfield& playfield) {
  std::ostringstream out;
  std::string playfieldName = identifier(playfield.name), upperName = upper(playfieldName);
  int rows = (int)playfield.rows.size();
  bool anyAsym = std::any_of(playfield.rows.begin(), playfield.rows.end(), [](const PfRow& row) { return row.asymmetric; });
  bool modes = std::any_of(playfield.rows.begin(), playfield.rows.end(), [&](const PfRow& row) { return row.mode != playfield.rows[0].mode; });
  bool pfVary = false, bkVary = false;
  for (int row = 1; row < rows; row++) pfVary |= playfield.pfAt(row) != playfield.pfAt(0), bkVary |= playfield.bkAt(row) != playfield.bkAt(0);
  out << "/* " << playfield.name << ": " << rows << " rows of " << playfield.lineHeight << " lines" << (anyAsym ? ", asymmetric rows" : "")
      << ". Rows: top first. */\n";
  out << "#define " << upperName << "_ROWS " << rows << "\n#define " << upperName << "_LINES " << playfield.lineHeight << "\n";
  out << "#define " << upperName << "_CTRLPF " << (playfield.rows[0].mode == PfMode::Mirror ? "0x01" : "0x00") << "  /* first row */\n";
  out << "#define " << upperName << "_COLUPF " << hex2(playfield.pfAt(0)) << "\n#define " << upperName << "_COLUBK " << hex2(playfield.bkAt(0)) << "\n";
  auto table = [&](const std::string& label, auto value, bool picture) {
    out << "static const uint8_t " << playfieldName << "_" << label << "[" << upperName << "_ROWS] = {\n";
    for (int row = 0; row < rows; row++) {
      out << "    " << value(row) << ",";
      if (picture) {
        out << "  /* |";
        for (int column = 0; column < 40; column++) out << (playfield.pixel(row, column) ? 'X' : '.');
        out << "| */";
      }
      out << "\n";
    }
    out << "};\n";
  };
  const char* registerNames[3] = {"pf0", "pf1", "pf2"};
  for (int i = 0; i < 3; i++)
    table(registerNames[i], [&](int row) { uint8_t leftRegisters[3], rightRegisters[3]; playfield.registers(row, leftRegisters, rightRegisters); return hex2(leftRegisters[i]); }, i == 0);
  if (anyAsym)
    for (int i = 0; i < 3; i++)
      table(std::string(registerNames[i]) + "r", [&](int row) { uint8_t leftRegisters[3], rightRegisters[3]; playfield.registers(row, leftRegisters, rightRegisters); return hex2(rightRegisters[i]); }, false);
  if (modes) table("ctrlpf", [&](int row) { return std::string(playfield.rows[row].mode == PfMode::Mirror ? "0x01" : "0x00"); }, false);
  if (pfVary) table("colupf", [&](int row) { return hex2(playfield.pfAt(row)); }, false);
  if (bkVary) table("colubk", [&](int row) { return hex2(playfield.bkAt(row)); }, false);
  return out.str();
}

static std::string cIncludeGuardedText(const Asset& asset, const std::string& sourceName, const std::string& fileName) {
  std::string guard = upper(identifier(fileName)) + "_H";
  std::string out = "/* Generated by VCSIDE from " + sourceName + ": edit that file, not this one. */\n";
  out += "#ifndef " + guard + "\n#define " + guard + "\n\n#include <stdint.h>\n\n";
  // shared by every .gfx.h of the project
  out += "#ifndef VCSGFX_TYPES\n#define VCSGFX_TYPES\n"
         "typedef struct { uint8_t frame, ticks; } vcsgfx_step;\n"
         "typedef struct { const vcsgfx_step *steps; uint8_t count, loop; } vcsgfx_animation;\n"
         "typedef struct { const vcsgfx_animation *animation; uint8_t step, timer; } vcsgfx_player;\n\n"
         "/* Starts an animation (nothing happens if it is already playing). */\n"
         "static inline void vcsgfx_play(vcsgfx_player *p, const vcsgfx_animation *a)\n"
         "{\n"
         "    if (p->animation == a) return;\n"
         "    p->animation = a;\n"
         "    p->step = 0;\n"
         "    p->timer = 0;\n"
         "}\n\n"
         "/* Once per video frame: advances the animation and returns the frame to draw. */\n"
         "static inline int vcsgfx_animate(vcsgfx_player *p)\n"
         "{\n"
         "    const vcsgfx_animation *a = p->animation;\n"
         "    if (!a || !a->count) return 0;\n"
         "    if (++p->timer >= a->steps[p->step].ticks) {\n"
         "        p->timer = 0;\n"
         "        if (p->step + 1 < a->count) p->step++;\n"
         "        else if (a->loop) p->step = 0;\n"
         "    }\n"
         "    return a->steps[p->step].frame;\n"
         "}\n"
         "#endif\n";
  for (auto& sprite : asset.sprites) out += "\n" + exportC(sprite);
  for (auto& playfield : asset.playfields) out += "\n" + exportC(playfield);
  out += "\n#endif\n";
  return out;
}
static std::string cIncludeGuarded(const Asset& asset, const std::string& sourceName, const std::string& fileName) {
  return indentWithTabs(cIncludeGuardedText(asset, sourceName, fileName), 4, false);
}

std::string cInclude(const Asset& asset, const std::string& sourceName) { return cIncludeGuarded(asset, sourceName, sourceName); }

bool generatedFrom(const std::string& firstLine, const std::string& sourceName) {
  return firstLine == "; Generated by VCSIDE from " + sourceName + ": edit that file, not this one." ||
         firstLine == "/* Generated by VCSIDE from " + sourceName + ": edit that file, not this one. */";
}

std::vector<GeneratedFile> generatedFiles(const Asset& asset, const std::string& sourceName, bool forC) {
  std::string stem = sourceName.substr(0, sourceName.rfind('.'));
  std::string ext = forC ? ".gfx.h" : ".gfx.asm";
  // the parts in the order of the asset; each its own file name
  std::vector<std::string> order;
  std::map<std::string, Asset> parts;
  auto part = [&](const std::string& key) -> Asset& {
    if (!parts.count(key)) order.push_back(key);
    return parts[key];
  };
  auto keyOf = [&](const std::string& name, const std::string& group) -> std::string {
    if (asset.split == Asset::Split::Each) return identifier(name);
    if (asset.split == Asset::Split::Group && !group.empty()) return identifier(group);
    return "";
  };
  for (auto& sprite : asset.sprites) part(keyOf(sprite.name, sprite.group)).sprites.push_back(sprite);
  for (auto& playfield : asset.playfields) part(keyOf(playfield.name, playfield.group)).playfields.push_back(playfield);
  if (order.empty()) part("");
  std::vector<GeneratedFile> out;
  for (auto& key : order) {
    GeneratedFile file;
    file.name = stem + (key.empty() ? "" : "." + key) + ext;
    file.text = forC ? cIncludeGuarded(parts[key], sourceName, file.name) : dasmInclude(parts[key], sourceName);
    out.push_back(std::move(file));
  }
  return out;
}


// The profile to use when the asset is for a kernel of one's own and still copied as
// batari Basic: the standard kernel with the options the graphics need.
static Profile basicFor(const Sprite& sprite, const Profile& profile) {
  if (profile.basic()) return profile;
  Profile basicProfile;
  basicProfile.kernel = Kernel::Standard;
  for (auto& layer : sprite.layers)
    if (coloursVary(layer)) (layer.obj == Obj::P0 ? basicProfile.options.playercolors : basicProfile.options.player1colors) = true;
  return basicProfile;
}
static Profile basicFor(const Playfield& playfield, const Profile& profile) {
  if (profile.basic()) return profile;
  Profile basicProfile;
  basicProfile.kernel = Kernel::Standard;
  for (int i = 1; i < (int)playfield.rows.size(); i++) basicProfile.options.pfcolors |= playfield.pfAt(i) != playfield.pfAt(0);
  basicProfile.options.pfcolors |= playfield.rows[0].pf >= 0;
  return basicProfile;
}

static std::string basicName(Obj object) {
  int number = playerNumber(object);
  if (number >= 0) return "player" + std::to_string(number);
  return object == Obj::M0 ? "missile0" : object == Obj::M1 ? "missile1" : "ball";
}

// The variable for a player's colour or size: COLUP0, _COLUP1 in the kernels with more
// players (their COLUP1 is the TIA's, rewritten down the screen), COLUP2...
static std::string playerVar(const char* registerName, int number, const Profile& profile) {
  if (number == 1 && profile.players() > 2) return "_" + std::string(registerName) + "1";
  return registerName + std::to_string(number);
}

static std::string basicSpriteText(const Sprite& whole, const Profile& requested, int frame) {
  Sprite sprite = whole;
  if (frame >= 0 && frame < whole.frames) {
    // just that frame
    for (auto& layer : sprite.layers) {
      layer.bits = {layer.bits[frame]};
      layer.colours = {layer.colours[frame]};
    }
    sprite.frames = 1;
  }
  Profile profile = basicFor(sprite, requested);
  std::ostringstream out;
  bool one = frame >= 0 && frame < whole.frames;
  if (one) out << "  rem " << sprite.name << ", frame " << frame + 1 << "; " << profile.describe() << "\n";
  else out << "  rem " << sprite.name << ": " << sprite.frames << " frame(s), " << sprite.phases() << " phase(s); " << profile.describe() << "\n";
  const Layer* first = nullptr;
  for (auto& layer : sprite.layers)
    if (isPlayer(layer.obj) && profile.has(layer.obj) && !first) first = &layer;
  for (int phase = 0; phase < sprite.phases(); phase++)
    for (int frameIndex = 0; frameIndex < sprite.frames; frameIndex++) {
      if (!one || sprite.phases() > 1) out << "  rem frame " << (one ? frame : frameIndex) + 1 << (sprite.phases() > 1 ? ", phase " + std::to_string(phase + 1) : "") << "\n";
      for (auto& layer : sprite.layers) {
        if (layer.phase != phase || !isPlayer(layer.obj) || !profile.has(layer.obj)) continue;
        std::string playerName = basicName(layer.obj);
        auto row = [&](int i) { return profile.topFirst() ? i : layer.height - 1 - i; };
        out << "  " << playerName << ":\n";
        for (int i = 0; i < layer.height; i++) out << "  " << bin8(layer.bits[frameIndex][row(i)]) << "\n";
        out << "end\n";
        if (profile.rowColours(layer.obj)) {
          out << "  " << playerName << "color:\n";
          for (int i = 0; i < layer.height; i++) out << "  " << hexColour(layer.colour(frameIndex, row(i))) << "\n";
          out << "end\n";
        } else {
          // the kernel and the score reload the colour registers: set them before every drawscreen
          out << "  " << playerVar("COLUP", playerNumber(layer.obj), profile) << " = " << hexColour(layer.colour(frameIndex, 0)) << " : rem every frame\n";
        }
      }
    }
  for (auto& layer : sprite.layers) {
    std::string objectName = basicName(layer.obj);
    if (!profile.has(layer.obj)) {
      out << "  rem " << objName(layer.obj) << ": not drawn by this kernel\n";
      continue;
    }
    int number = playerNumber(layer.obj);
    if (number >= 0) {
      int nusiz = (layer.size == 4 ? 7 : layer.size == 2 ? 5 : 0);
      bool refpInNusiz = profile.kernel == Kernel::DPCPlus || profile.kernel == Kernel::PXE;  // their NUSIZ bit 3 reflects
      if (refpInNusiz && layer.reflect) nusiz |= 8;
      if (nusiz) out << "  " << playerVar("NUSIZ", number, profile) << " = $" << std::hex << std::uppercase << nusiz << std::dec << "\n";
      if (layer.reflect && !refpInNusiz) {
        if (number <= 1 && profile.kernel == Kernel::Standard) out << "  REFP" << number << " = 8\n";
        else out << "  rem " << objectName << ": reflected\n";
      }
    } else if (profile.kernel != Kernel::Multisprite || layer.obj == Obj::BL) {
      int blockHeight = 0;
      for (int i = 0; i < layer.height; i++) blockHeight += rowHasPixels(layer, 0, i);
      out << "  " << objectName << "height = " << blockHeight << "\n";
    }
    if (first && &layer != first)
      out << "  rem " << objectName << "x = " << basicName(first->obj) << "x + " << layer.dx - first->dx << " : " << objectName << "y = "
          << basicName(first->obj) << "y + " << layer.dy - first->dy << "\n";
  }
  return out.str();
}

static std::string basicAnimationText(const Sprite& sprite, const Animation& animation, const Profile& requested) {
  Profile profile = basicFor(sprite, requested);
  std::ostringstream out;
  std::string prefix = identifier(sprite.name) + "_" + identifier(animation.name), timerName = prefix + "_t", stepName = prefix + "_s";
  int stepCount = (int)animation.steps.size();
  out << "  rem " << sprite.name << ", animation " << animation.name << ": " << stepCount << " steps" << (animation.loop ? ", loops" : "")
      << ". gosub " << prefix << " once per frame; gosub " << prefix << "_reset restarts it\n";
  if (stepCount == 0) return out.str() + "  rem (no steps)\n";
  if (!animation.basicTimer.empty() && !animation.basicStep.empty())
    out << "  dim " << timerName << " = " << animation.basicTimer << "\n  dim " << stepName << " = " << animation.basicStep << "\n";
  else
    out << "  rem dim " << timerName << " and " << stepName << " to two free variables (the animation's batari Basic variables)\n";
  out << "  data " << prefix << "_ticks\n  ";
  for (int i = 0; i < stepCount; i++) out << (i ? ", " : "") << std::clamp(animation.steps[i].ticks, 1, 255);
  out << "\nend\n  goto " << prefix << "_end\n";
  out << prefix << "_reset\n  " << timerName << " = 0 : " << stepName << " = 0\n  return\n";
  out << prefix << "\n  " << timerName << " = " << timerName << " + 1\n  temp1 = " << prefix << "_ticks[" << stepName << "]\n";
  out << "  if " << timerName << " < temp1 then goto " << prefix << "_show\n  " << timerName << " = 0\n  " << stepName << " = " << stepName << " + 1\n";
  out << "  if " << stepName << " >= " << stepCount << " then " << stepName << " = " << (animation.loop ? 0 : stepCount - 1) << "\n";
  out << prefix << "_show\n  on " << stepName << " goto";
  for (auto& step : animation.steps) out << " " << prefix << "_f" << step.frame;
  out << "\n";
  std::set<int> frames;
  for (auto& step : animation.steps) frames.insert(std::clamp(step.frame, 0, sprite.frames - 1));
  for (int frame : frames) out << prefix << "_f" << frame << "\n" << exportBasic(sprite, profile, frame) << "  return\n";
  out << prefix << "_end\n";
  return out.str();
}

static std::string basicPlayfieldText(const Playfield& playfield, const Profile& requested) {
  Profile profile = basicFor(playfield, requested);
  std::ostringstream out;
  int rows = (int)playfield.rows.size();
  out << "  rem " << playfield.name << "; " << profile.describe() << "\n  playfield:\n";
  int firstColumn = profile.pfFirst(), endColumn = firstColumn + profile.pfColumns();
  for (int row = 0; row < rows; row++) {
    out << "  ";
    for (int column = firstColumn; column < endColumn; column++) out << (playfield.pixel(row, column) ? 'X' : '.');
    out << "\n";
  }
  out << "end\n";
  auto table = [&](const char* block, auto colour) {
    out << "  " << block << ":\n";
    for (int row = 0; row < rows; row++) out << "  " << hexColour(colour(row)) << "\n";
    out << "end\n";
  };
  bool bkVary = false;
  for (int row = 1; row < rows; row++) bkVary |= playfield.bkAt(row) != playfield.bkAt(0);
  if (profile.pfRowColours()) table("pfcolors", [&](int row) { return playfield.pfAt(row); });
  else out << "  COLUPF = " << hexColour(playfield.pfAt(0)) << "\n";
  if (profile.kernel == Kernel::Standard && profile.bkRowColours()) table("pfcolors", [&](int row) { return playfield.bkAt(row); });  // option background: to COLUBK
  else if (profile.bkRowColours() && bkVary) table("bkcolors", [&](int row) { return playfield.bkAt(row); });
  else out << "  COLUBK = " << hexColour(playfield.bkAt(0)) << "\n";
  if (profile.kernel == Kernel::DPCPlus || profile.kernel == Kernel::PXE) {
    // the height of the rows: the data fetchers advance FRACINC/256 rows a line, over 176 lines
    int inc = std::clamp((int)std::lround(rows * 256.0 / 176), 1, 255);
    std::string incrementText = std::to_string(inc);
    out << "  rem row height (" << rows << " rows), before every drawscreen:\n  DF0FRACINC = " << incrementText << " : DF1FRACINC = " << incrementText
        << " : DF2FRACINC = " << incrementText << " : DF3FRACINC = " << incrementText << " : DF4FRACINC = " << incrementText << " : DF6FRACINC = " << incrementText << "\n";
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// Regions of a batari Basic program kept up to date

static std::string trimmed(const std::string& text) {
  size_t first = text.find_first_not_of(" \t\r"), last = text.find_last_not_of(" \t\r");
  return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}
static std::string lowered(std::string text) {
  for (auto& character : text) character = (char)std::tolower((unsigned char)character);
  return text;
}

// "rem vcsgfx art.vcsgfx hero frame 2" -> file, item, frame (-1); "rem vcsgfx end" -> end
struct Marker {
  bool is = false, end = false;
  std::string file, item, animation;
  int frame = -1;
};
static Marker marker(const std::string& line) {
  Marker result;
  std::istringstream stream(trimmed(line));
  std::string rem, tag;
  stream >> rem >> tag;
  if (lowered(rem) != "rem" || lowered(tag) != "vcsgfx") return result;
  result.is = true;
  std::vector<std::string> words;
  for (std::string word; stream >> word;) words.push_back(word);
  if (words.size() == 1 && lowered(words[0]) == "end") return result.end = true, result;
  if (words.empty()) return result;
  result.file = words[0];
  size_t wordCount = words.size();
  if (wordCount >= 3 && lowered(words[wordCount - 2]) == "frame") result.frame = std::atoi(words[wordCount - 1].c_str()) - 1, wordCount -= 2;
  for (size_t i = 2; i < wordCount; i++)
    if (lowered(words[i]) == "animation") {
      for (size_t j = i + 1; j < wordCount; j++) result.animation += (j > i + 1 ? " " : "") + words[j];
      wordCount = i;
      break;
    }
  for (size_t i = 1; i < wordCount; i++) result.item += (i > 1 ? " " : "") + words[i];
  return result;
}

static std::string regionBody(const Asset& asset, const std::string& item, int frame, const std::string& animation, const Profile& profile,
                              bool* found) {
  *found = true;
  for (auto& sprite : asset.sprites)
    if (sprite.name == item) {
      if (animation.empty()) return exportBasic(sprite, profile, frame);
      for (auto& candidate : sprite.animations)
        if (candidate.name == animation) return exportBasicAnimation(sprite, candidate, profile);
      *found = false;
      return "";
    }
  for (auto& playfield : asset.playfields)
    if (playfield.name == item) return exportBasic(playfield, profile);
  *found = false;
  return "";
}

std::string basicRegion(const std::string& file, const Asset& asset, int item, const Profile& profile) {
  std::string out;
  bool found;
  if (item < (int)asset.sprites.size()) {
    const Sprite& sprite = asset.sprites[item];
    if (!sprite.animations.empty()) {
      for (size_t i = 0; i < sprite.animations.size(); i++)
        out += (i ? "\n" : "") + std::string("\trem vcsgfx ") + file + " " + sprite.name + " animation " + sprite.animations[i].name + "\n" +
               exportBasicAnimation(sprite, sprite.animations[i], profile) + "\trem vcsgfx end\n";
      return out;
    }
    for (int frameIndex = 0; frameIndex < sprite.frames; frameIndex++) {
      if (frameIndex) out += "\n";
      std::string frame = sprite.frames > 1 ? " frame " + std::to_string(frameIndex + 1) : "";
      out += "\trem vcsgfx " + file + " " + sprite.name + frame + "\n" + regionBody(asset, sprite.name, sprite.frames > 1 ? frameIndex : -1, "", profile, &found) + "\trem vcsgfx end\n";
    }
  } else if (item - asset.sprites.size() < asset.playfields.size()) {
    const Playfield& playfield = asset.playfields[item - asset.sprites.size()];
    out = "\trem vcsgfx " + file + " " + playfield.name + "\n" + regionBody(asset, playfield.name, -1, "", profile, &found) + "\trem vcsgfx end\n";
  }
  return out;
}

std::string updateBasicRegions(const std::string& source, const std::string& file, const Asset& asset, const Profile& profile,
                               int* updated, std::vector<std::string>* problems) {
  if (updated) *updated = 0;
  bool crlf = source.find("\r\n") != std::string::npos;
  std::vector<std::string> lines;
  {
    std::string current;
    for (char character : source) {
      if (character == '\n') lines.push_back(current), current.clear();
      else current += character;
    }
    lines.push_back(current);  // what follows the last newline ("" if the text ends with one)
  }
  std::vector<std::string> out;
  for (size_t i = 0; i < lines.size(); i++) {
    out.push_back(lines[i]);
    Marker region = marker(lines[i]);
    if (!region.is || region.end || lowered(region.file) != lowered(file)) continue;
    size_t end = i + 1;
    while (end < lines.size() && !marker(lines[end]).end) end++;
    if (end >= lines.size()) {
      if (problems) problems->push_back(F("Line {0}: `rem vcsgfx {1}` has no `rem vcsgfx end`.", {std::to_string(i + 1), region.file + " " + region.item}));
      continue;
    }
    auto foundSprite = std::find_if(asset.sprites.begin(), asset.sprites.end(), [&](const Sprite& sprite) { return sprite.name == region.item; });
    if (foundSprite != asset.sprites.end() && region.frame >= foundSprite->frames) {
      if (problems) problems->push_back(F("Line {0}: {1} has {2} frames.", {std::to_string(i + 1), region.item, std::to_string(foundSprite->frames)}));
      continue;
    }
    bool found;
    std::string body = regionBody(asset, region.item, region.frame, region.animation, profile, &found);
    if (!found) {
      if (problems)
        problems->push_back(region.animation.empty() || foundSprite == asset.sprites.end()
                                ? F("Line {0}: {1} has no sprite or playfield called {2}.", {std::to_string(i + 1), file, region.item})
                                : F("Line {0}: {1} has no animation called {2}.", {std::to_string(i + 1), region.item, region.animation}));
      continue;
    }
    std::istringstream bodyStream(body);
    for (std::string bodyLine; std::getline(bodyStream, bodyLine);) out.push_back(crlf ? bodyLine + "\r" : bodyLine);
    i = end - 1;  // the end marker is copied on the next turn
    if (updated) ++*updated;
  }
  std::string text;
  for (size_t i = 0; i < out.size(); i++) text += out[i] + (i + 1 < out.size() ? "\n" : "");
  return text;
}

// ---------------------------------------------------------------------------
// Import

namespace {
struct Block {
  std::string label, kind;     // kind: a batari Basic block ("player0", "pfcolors"...) or "" for assembly
  std::vector<int> values;     // bytes; playfield rows as -1 with the row in text
  std::vector<std::string> rows;
  std::vector<int> comments;   // ";$xx" after a value, -1 if none
};

int parseValue(const std::string& text) {
  std::string trimmed = text;
  while (!trimmed.empty() && (trimmed[0] == '#' || std::isspace((unsigned char)trimmed[0]))) trimmed.erase(0, 1);
  try {
    if (!trimmed.empty() && trimmed[0] == '%') return std::stoi(trimmed.substr(1), nullptr, 2);
    if (!trimmed.empty() && trimmed[0] == '$') return std::stoi(trimmed.substr(1), nullptr, 16);
    if (trimmed.rfind("0x", 0) == 0) return std::stoi(trimmed.substr(2), nullptr, 16);
    if (!trimmed.empty() && std::isdigit((unsigned char)trimmed[0])) return std::stoi(trimmed);
  } catch (...) {}
  return -1;
}
}  // namespace

bool importText(const std::string& text, Asset* out, std::string* what, const Profile& hint) {
  // a whole program says its kernel itself
  Profile kernel = hint;
  if (std::regex_search(text, std::regex(R"(\bset\s+kernel\s)", std::regex::icase))) kernel = profileFromBasic(text);
  static const std::regex bbStart(R"(^\s*(?:if\b.*\bthen\s+)?(player(?:1[0-6]|[0-9])(?:-\d+)?(?:color)?|playfield|pfcolors|bkcolors)\s*:\s*$)", std::regex::icase);
  // data lines; .byte and .db also in the first column, where other tools write them
  static const std::regex asmData(R"(^(?:\s+(?:\.byte|dc\.b|\.db|byte)|\s*(?:\.byte|\.db))\s+(.*)$)", std::regex::icase);
  // the header of another playfield editor's export: "; mode: asymmetric mirrored line-height 8"
  static const std::regex modeHeader(R"(^\s*;\s*mode:(.*)$)", std::regex::icase);
  struct { bool seen = false, asym = false, repeat = false; int lineHeight = 0; } mode;
  static const std::regex label(R"(^([A-Za-z_.][\w.]*):?\s*(?:;.*)?$)");
  static const std::regex colourComment(R"(;\s*\$([0-9A-Fa-f]{2}))");
  std::vector<Block> blocks;
  bool topFirst = false, sawRowsNote = false;
  std::istringstream lineStream(text);
  std::string line;
  Block* openBlock = nullptr;
  while (std::getline(lineStream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch match;
    if (line.find("Rows: top first") != std::string::npos) topFirst = true, sawRowsNote = true;
    if (!openBlock && std::regex_match(line, match, modeHeader)) {
      std::string modeText = match[1].str();
      for (auto& character : modeText) character = (char)std::tolower((unsigned char)character);
      static const std::regex word(R"([a-z]+(?:-?height)?:?\s*\d*)");
      for (auto tokenIt = std::sregex_iterator(modeText.begin(), modeText.end(), word); tokenIt != std::sregex_iterator(); ++tokenIt) {
        std::string token = tokenIt->str();
        if (token.rfind("asym", 0) == 0) mode.asym = true;
        else if (token.rfind("repeat", 0) == 0) mode.repeat = true;
        else if (token.rfind("line", 0) == 0 && token.find_first_of("0123456789") != std::string::npos)
          mode.lineHeight = std::stoi(token.substr(token.find_first_of("0123456789")));
      }
      mode.seen = true;
      continue;
    }
    if (line.find("Rows: bottom first") != std::string::npos) topFirst = false, sawRowsNote = true;
    if (openBlock) {
      std::string trimmedLine = line;
      trimmedLine.erase(0, trimmedLine.find_first_not_of(" \t"));
      if (trimmedLine.rfind("end", 0) == 0 && (trimmedLine.size() == 3 || std::isspace((unsigned char)trimmedLine[3]))) { openBlock = nullptr; continue; }
      if (trimmedLine.empty()) continue;
      if (openBlock->kind == "playfield") openBlock->rows.push_back(trimmedLine.substr(0, trimmedLine.find_first_of(" \t")));
      else if (int value = parseValue(trimmedLine.substr(0, trimmedLine.find_first_of(" \t;"))); value >= 0) openBlock->values.push_back(value), openBlock->comments.push_back(-1);
      continue;
    }
    if (std::regex_match(line, match, bbStart)) {
      Block block;
      block.kind = match[1].str();
      for (auto& character : block.kind) character = (char)std::tolower((unsigned char)character);
      blocks.push_back(block);
      openBlock = &blocks.back();
      continue;
    }
    if (std::regex_match(line, match, asmData)) {
      if (blocks.empty() || !blocks.back().kind.empty()) blocks.push_back(Block{});
      Block& block = blocks.back();
      std::string data = match[1].str();
      std::smatch colourMatch;
      int comment = std::regex_search(data, colourMatch, colourComment) ? std::stoi(colourMatch[1].str(), nullptr, 16) : -1;
      data = data.substr(0, data.find(';'));
      std::istringstream valueStream(data);
      for (std::string piece; std::getline(valueStream, piece, ',');)
        if (int byteValue = parseValue(piece); byteValue >= 0) block.values.push_back(byteValue), block.comments.push_back(comment);
      continue;
    }
    if (std::regex_match(line, match, label) && line.find('=') == std::string::npos) {
      Block block;
      block.label = match[1].str();
      blocks.push_back(block);
    }
  }
  blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](const Block& block) { return block.values.empty() && block.rows.empty(); }), blocks.end());
  if (blocks.empty()) {
    if (what) what->clear();
    return false;
  }
  auto lower = [](std::string text) {
    for (auto& character : text) character = (char)std::tolower((unsigned char)character);
    return text;
  };
  auto endsWith = [](const std::string& text, const std::string& suffix) { return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0; };
  std::vector<std::string> found;

  // batari Basic playfield
  for (auto& block : blocks)
    if (block.kind == "playfield") {
      Playfield playfield;
      playfield.rows.assign(block.rows.size(), PfRow{});
      for (size_t i = 0; i < block.rows.size(); i++) {
        if (block.rows[i].size() <= 16) {  // the multisprite kernel: the left half from column 4, mirrored
          playfield.rows[i].bits = (rowBits(block.rows[i], false) & 0xFFFF) << 4;
          playfield.rows[i].mode = PfMode::Mirror;
          continue;
        }
        playfield.rows[i].asymmetric = true;
        playfield.rows[i].bits = rowBits(block.rows[i], false) << 4;  // bB columns are the screen's 4-35
      }
      for (auto& other : blocks) {
        if (other.kind != "pfcolors" && other.kind != "bkcolors") continue;
        for (size_t i = 0; i < other.values.size() && i < playfield.rows.size(); i++) (other.kind == "pfcolors" ? playfield.rows[i].pf : playfield.rows[i].bk) = other.values[i];
      }
      // rows whose right half is what a mirror or a repeat would give are symmetric again
      for (int row = 0; row < (int)playfield.rows.size(); row++)
        for (PfMode symmetricMode : {PfMode::Mirror, PfMode::Repeat}) {
          Playfield candidate = playfield;
          candidate.rows[row].asymmetric = false;
          candidate.rows[row].mode = symmetricMode;
          candidate.rows[row].bits &= 0xFFFFFull;
          bool same = true;
          for (int column = 20; column < 40; column++) same &= candidate.pixel(row, column) == playfield.pixel(row, column);
          if (same) { playfield.rows[row] = candidate.rows[row]; break; }
        }
      playfield.name = "playfield";
      out->playfields.push_back(playfield);
      found.push_back(F("a playfield of {0} rows", {std::to_string(playfield.rows.size())}));
      break;
    }

  // batari Basic players: each block is a frame, colours pair in order. The standard and
  // multisprite kernels read the rows bottom first, DPC+ and PXE top first.
  {
    Sprite sprite;
    sprite.name = "sprite";
    bool top = kernel.topFirst() && kernel.basic();
    for (int number = 0; number < 17; number++) {
      Obj object = playerObj(number);
      std::string kindName = "player" + std::to_string(number);
      std::vector<const Block*> dataBlocks, colourBlocks;
      for (auto& block : blocks) {
        std::string kind = block.kind;
        if (size_t dash = kind.find('-'); dash != std::string::npos) kind.erase(dash, kind.find_first_not_of("0123456789", dash + 1) - dash);  // player1-3: the first
        if (kind == kindName) dataBlocks.push_back(&block);
        if (kind == kindName + "color") colourBlocks.push_back(&block);
      }
      if (dataBlocks.empty()) continue;
      Layer layer;
      layer.obj = object;
      int rowCount = 0;
      for (auto* block : dataBlocks) rowCount = std::max(rowCount, (int)block->values.size());
      sprite.frames = std::max(sprite.frames, (int)dataBlocks.size());
      layer.resize((int)dataBlocks.size(), rowCount);
      for (size_t frame = 0; frame < dataBlocks.size(); frame++) {
        int count = (int)dataBlocks[frame]->values.size();
        for (int i = 0; i < count; i++) layer.bits[frame][top ? i : rowCount - 1 - i] = (uint8_t)dataBlocks[frame]->values[i];
        if (frame < colourBlocks.size())
          for (int i = 0; i < (int)colourBlocks[frame]->values.size() && i < rowCount; i++) layer.colours[frame][top ? i : rowCount - 1 - i] = colourBlocks[frame]->values[i];
      }
      sprite.layers.push_back(layer);
    }
    if (!sprite.layers.empty()) {
      sprite.setFrames(sprite.frames);
      out->sprites.push_back(sprite);
      found.push_back(F("{0} player(s), {1} frame(s)", {std::to_string(sprite.layers.size()), std::to_string(sprite.frames)}));
    }
  }

  // assembly playfield tables: labels ending in PF0, PF1, PF2 (and PF0R... for the right half)
  {
    std::map<std::string, const Block*> registerTables;
    for (auto& block : blocks) {
      if (!block.kind.empty()) continue;
      std::string lowerLabel = lower(block.label);
      for (const char* suffix : {"pf0r", "pf1r", "pf2r", "pf0", "pf1", "pf2", "colupf_rows", "colubk_rows", "ctrlpf_rows"})
        if (endsWith(lowerLabel, suffix) && !registerTables.count(suffix)) { registerTables[suffix] = &block; break; }
    }
    // the registers of each row: PF0-PF2 and, for asymmetric rows, PF0R-PF2R
    std::vector<int> registerValues[6];
    bool hasRight = false;
    std::string name;
    if (registerTables.count("pf0") && registerTables.count("pf1") && registerTables.count("pf2")) {
      const char* keys[6] = {"pf0", "pf1", "pf2", "pf0r", "pf1r", "pf2r"};
      hasRight = registerTables.count("pf0r") && registerTables.count("pf1r") && registerTables.count("pf2r");
      for (int registerIndex = 0; registerIndex < (hasRight ? 6 : 3); registerIndex++) registerValues[registerIndex] = registerTables[keys[registerIndex]]->values;
      if (mode.asym && !hasRight)  // one table per register, left and right halves of each row in turn
        for (int registerIndex = 0; registerIndex < 3; registerIndex++) {
          std::vector<int> both = registerValues[registerIndex];
          registerValues[registerIndex].clear();
          for (size_t i = 0; i + 1 < both.size(); i += 2) registerValues[registerIndex].push_back(both[i]), registerValues[registerIndex + 3].push_back(both[i + 1]);
          hasRight = true;
        }
      name = registerTables["pf0"]->label;
      name = name.substr(0, name.size() - 3);
    } else if (mode.seen) {
      // one line per row: PF0, PF1, PF2 (and the right half's three)
      for (auto& block : blocks) {
        if (!block.kind.empty() || !block.rows.empty()) continue;
        size_t perRow = mode.asym ? 6 : 3;
        if (block.values.size() < perRow || block.values.size() % perRow) continue;
        for (size_t i = 0; i < block.values.size(); i++) registerValues[i % perRow].push_back(block.values[i]);
        hasRight = mode.asym;
        name = block.label;
        break;
      }
    }
    if (!registerValues[0].empty()) {
      Playfield playfield;
      int rows = (int)std::min({registerValues[0].size(), registerValues[1].size(), registerValues[2].size()});
      playfield.rows.assign(rows, PfRow{});
      if (mode.lineHeight > 0) playfield.lineHeight = std::clamp(mode.lineHeight, 1, 64);
      bool flip = sawRowsNote && !topFirst;
      for (int i = 0; i < rows; i++) {
        int rowIndex = flip ? rows - 1 - i : i;
        PfRow& row = playfield.rows[rowIndex];
        if (mode.seen) row.mode = mode.repeat ? PfMode::Repeat : PfMode::Mirror;
        if (registerTables.count("ctrlpf_rows") && i < (int)registerTables["ctrlpf_rows"]->values.size())
          row.mode = registerTables["ctrlpf_rows"]->values[i] & 1 ? PfMode::Mirror : PfMode::Repeat;
        int leftRegisters[3] = {registerValues[0][i], registerValues[1][i], registerValues[2][i]}, registerIndex, bitIndex;
        for (int j = 0; j < 20; j++)
          if (pfBit(j, &registerIndex, &bitIndex), (leftRegisters[registerIndex] >> bitIndex) & 1) row.bits |= 1ull << j;
        if (hasRight && i < (int)std::min({registerValues[3].size(), registerValues[4].size(), registerValues[5].size()})) {
          int rightRegisters[3] = {registerValues[3][i], registerValues[4][i], registerValues[5][i]};
          if (rightRegisters[0] != leftRegisters[0] || rightRegisters[1] != leftRegisters[1] || rightRegisters[2] != leftRegisters[2]) {
            row.asymmetric = true;
            for (int column = 20; column < 40; column++) {
              int sourceColumn = row.mode == PfMode::Mirror ? 39 - column : column - 20;
              if (pfBit(sourceColumn, &registerIndex, &bitIndex), (rightRegisters[registerIndex] >> bitIndex) & 1) row.bits |= 1ull << column;
            }
          }
        }
        if (registerTables.count("colupf_rows") && i < (int)registerTables["colupf_rows"]->values.size()) row.pf = registerTables["colupf_rows"]->values[i];
        if (registerTables.count("colubk_rows") && i < (int)registerTables["colubk_rows"]->values.size()) row.bk = registerTables["colubk_rows"]->values[i];
      }
      while (!name.empty() && (name.back() == '_' || name.back() == '.')) name.pop_back();
      playfield.name = name.empty() ? "playfield" : name;
      out->playfields.push_back(playfield);
      found.push_back(F("a playfield of {0} rows", {std::to_string(rows)}));
    }
  }

  // assembly sprite tables: every other labelled table of bytes is a frame; tables whose
  // label says colour are the colours of the frames in order. A ";$xx" after a row is its colour.
  {
    // VCSIDE's own labels: <sprite>_<object>[n][_<layer name>]_Frame<f> / _Colour<f>
    std::map<std::string, std::vector<const Block*>> frames, colours;  // by label prefix
    std::map<std::string, const Block*> animFrames, animTicks;          // <sprite>_<animation>
    std::vector<std::string> order;
    static const std::regex ours(R"(^(.*)_(?:Frame|Colour)\d+$)");
    static const std::regex colourLabel("colou?r", std::regex::icase);
    for (auto& block : blocks) {
      if (!block.kind.empty() || mode.seen) continue;  // a "; mode:" export holds a playfield only
      std::string lowerLabel = lower(block.label);
      bool isPf = false;
      for (const char* suffix : {"pf0r", "pf1r", "pf2r", "pf0", "pf1", "pf2", "colupf_rows", "colubk_rows", "ctrlpf_rows"}) isPf |= endsWith(lowerLabel, suffix);
      if (isPf) continue;
      // frame pointers are not frames; animation tables are read below
      static const std::regex pointers(R"(_(?:Frame|Colour)Ptr(?:Lo|Hi)$)");
      static const std::regex animTable(R"(^(.*)_(FRAME|TICKS)$)");
      std::smatch animationMatch;
      if (std::regex_search(block.label, pointers)) continue;
      if (std::regex_match(block.label, animationMatch, animTable)) {
        (animationMatch[2] == "FRAME" ? animFrames : animTicks)[animationMatch[1].str()] = &block;
        continue;
      }
      std::smatch match;
      std::string key = std::regex_match(block.label, match, ours) && match[1].str().find('_') != std::string::npos ? match[1].str() : "";
      bool colour = std::regex_search(block.label, colourLabel);
      if (std::find(order.begin(), order.end(), key) == order.end()) order.push_back(key);
      (colour ? colours : frames)[key].push_back(&block);
    }
    Sprite sprite;
    sprite.name.clear();
    for (auto& key : order) {
      auto& frameBlocks = frames[key];
      if (frameBlocks.empty()) continue;
      Layer layer;
      std::smatch match;
      static const std::regex obj(R"(^(.*?)_(P1[0-6]|P[0-9]|M0|M1|BL)[b-z]?(?:_(\w+))?$)");
      if (std::regex_match(key, match, obj)) {
        for (int objectIndex = 0; objectIndex < kObjects; objectIndex++)
          if (match[2].str() == objName((Obj)objectIndex)) layer.obj = (Obj)objectIndex;
        layer.name = match[3].str();
        if (sprite.name.empty()) sprite.name = match[1].str();
      }
      int rowCount = 0;
      for (auto* block : frameBlocks) rowCount = std::max(rowCount, (int)block->values.size());
      layer.resize((int)frameBlocks.size(), rowCount);
      bool flip = !sawRowsNote || !topFirst;  // assembly tables are bottom first unless they say otherwise
      for (size_t frameIndex = 0; frameIndex < frameBlocks.size(); frameIndex++) {
        int count = (int)frameBlocks[frameIndex]->values.size();
        for (int i = 0; i < count; i++) {
          int row = flip ? rowCount - 1 - i : i;
          int value = frameBlocks[frameIndex]->values[i];
          layer.bits[frameIndex][row] = (uint8_t)(isPlayer(layer.obj) ? value : (value & 2) != 0);
          if (frameBlocks[frameIndex]->comments[i] >= 0) layer.colours[frameIndex][row] = frameBlocks[frameIndex]->comments[i];
        }
        auto& colourBlocks = colours[key];
        if (frameIndex < colourBlocks.size())
          for (int i = 0; i < (int)colourBlocks[frameIndex]->values.size() && i < rowCount; i++) layer.colours[frameIndex][flip ? rowCount - 1 - i : i] = colourBlocks[frameIndex]->values[i];
      }
      // the equates of an exported sprite give the position, size and phase back
      if (!key.empty()) {
        std::istringstream again(text);
        for (std::string equateLine; std::getline(again, equateLine);) {
          if (equateLine.rfind(key + "_", 0) != 0) continue;
          std::string rest = equateLine.substr(key.size() + 1);
          std::smatch equate;
          static const std::regex eq(R"(^(DX|DY|PHASE)\s*=\s*(-?\d+))");
          if (std::regex_search(rest, equate, eq)) {
            int value = std::stoi(equate[2]);
            (equate[1] == "DX" ? layer.dx : equate[1] == "DY" ? layer.dy : layer.phase) = equate[1] == "PHASE" ? std::clamp(value, 0, 3) : value;
          }
          if (rest.rfind("NUSIZ", 0) == 0) layer.size = rest.find("$07") != std::string::npos ? 4 : rest.find("$05") != std::string::npos ? 2 : 1;
          if (rest.rfind("REFP", 0) == 0) layer.reflect = true;
          if (rest.rfind("SIZE", 0) == 0) layer.size = rest.find("$30") != std::string::npos ? 8 : rest.find("$20") != std::string::npos ? 4 : rest.find("$10") != std::string::npos ? 2 : 1;
        }
      }
      sprite.frames = std::max(sprite.frames, (int)frameBlocks.size());
      sprite.layers.push_back(layer);
    }
    if (!sprite.layers.empty()) {
      if (sprite.name.empty()) sprite.name = "sprite";
      sprite.setFrames(sprite.frames);
      for (auto& [prefix, frameBlock] : animFrames) {
        if (prefix.rfind(sprite.name + "_", 0) != 0) continue;
        Animation animation;
        animation.name = prefix.substr(sprite.name.size() + 1);
        const Block* tickBlock = animTicks.count(prefix) ? animTicks[prefix] : nullptr;
        for (size_t i = 0; i < frameBlock->values.size(); i++)
          animation.steps.push_back({std::clamp(frameBlock->values[i], 0, sprite.frames - 1), tickBlock && i < tickBlock->values.size() ? std::clamp(tickBlock->values[i], 1, 255) : 8});
        animation.loop = text.find(prefix + "_LOOP = 0") == std::string::npos;
        sprite.animations.push_back(animation);
      }
      out->sprites.push_back(sprite);
      found.push_back(F("{0} layer(s), {1} frame(s)", {std::to_string(sprite.layers.size()), std::to_string(sprite.frames)}));
    }
  }
  if (what) {
    what->clear();
    for (auto& item : found) *what += (what->empty() ? "" : "; ") + item;
  }
  return !found.empty();
}

// The exports, indented with tabs like the templates
std::string exportDasm(const Sprite& sprite, const ExportOptions& options) { return indentWithTabs(dasmSpriteText(sprite, options), 8, true); }
std::string exportDasm(const Playfield& playfield, const ExportOptions& options) { return indentWithTabs(dasmPlayfieldText(playfield, options), 8, true); }
std::string exportC(const Sprite& sprite) { return indentWithTabs(cSpriteText(sprite), 4, false); }
std::string exportC(const Playfield& playfield) { return indentWithTabs(cPlayfieldText(playfield), 4, false); }
std::string exportBasic(const Sprite& whole, const Profile& requested, int frame) { return indentWithTabs(basicSpriteText(whole, requested, frame), 2, true); }
std::string exportBasicAnimation(const Sprite& sprite, const Animation& animation, const Profile& requested) {
  return indentWithTabs(basicAnimationText(sprite, animation, requested), 2, true);
}
std::string exportBasic(const Playfield& playfield, const Profile& requested) { return indentWithTabs(basicPlayfieldText(playfield, requested), 2, true); }

}  // namespace vcs::gfx
