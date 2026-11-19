// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/creative_tools.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "gfx.h"
#include "music/export.h"
#include "music/song.h"
#include "music/tuning.h"

namespace vcs::assistant {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

ToolResult fail(const std::string& message) {
  ToolResult result;
  result.error = true;
  result.text = "Error: " + message;
  return result;
}

ToolResult ok(const std::string& text) {
  ToolResult result;
  result.text = text;
  return result;
}

std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return text;
}

bool endsWith(const std::string& text, const std::string& ending) { return text.size() >= ending.size() && text.compare(text.size() - ending.size(), ending.size(), ending) == 0; }

std::string textOf(const json& args, const char* key, const std::string& fallback = "") {
  if (!args.is_object() || !args.contains(key)) return fallback;
  const json& value = args[key];
  if (value.is_string()) return value.get<std::string>();
  if (value.is_number_integer()) return std::to_string(value.get<long long>());
  if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
  return fallback;
}

bool has(const json& args, const char* key) { return args.is_object() && args.contains(key) && !args[key].is_null(); }

int numberOf(const json& args, const char* key, int fallback) {
  if (!has(args, key)) return fallback;
  const json& value = args[key];
  if (value.is_number()) return (int)value.get<double>();
  if (value.is_string()) {
    try {
      return std::stoi(value.get<std::string>());
    } catch (...) {
    }
  }
  return fallback;
}

bool flagOf(const json& args, const char* key, bool fallback) {
  if (!has(args, key)) return fallback;
  const json& value = args[key];
  if (value.is_boolean()) return value.get<bool>();
  if (value.is_number()) return value.get<double>() != 0;
  std::string text = lowered(textOf(args, key));
  if (text == "true" || text == "yes" || text == "1" || text == "on") return true;
  if (text == "false" || text == "no" || text == "0" || text == "off") return false;
  return fallback;
}

// A list of texts: an array, or one text with a row per line.
std::vector<std::string> linesOf(const json& args, const char* key) {
  std::vector<std::string> lines;
  if (!has(args, key)) return lines;
  const json& value = args[key];
  if (value.is_array()) {
    for (auto& item : value) lines.push_back(item.is_string() ? item.get<std::string>() : item.dump());
  } else if (value.is_string()) {
    std::istringstream in(value.get<std::string>());
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  return lines;
}

// A list of numbers: an array, or a text with numbers separated by spaces or commas ("15 12 9").
bool numbersOf(const json& args, const char* key, std::vector<int>* numbers) {
  numbers->clear();
  if (!has(args, key)) return false;
  const json& value = args[key];
  if (value.is_array()) {
    for (auto& item : value) {
      if (item.is_number()) numbers->push_back((int)item.get<double>());
      else if (item.is_string()) {
        try {
          numbers->push_back(std::stoi(item.get<std::string>(), nullptr, 0));
        } catch (...) {
          return false;
        }
      } else return false;
    }
    return true;
  }
  if (value.is_number()) {
    numbers->push_back((int)value.get<double>());
    return true;
  }
  if (!value.is_string()) return false;
  std::string text = value.get<std::string>();
  for (auto& c : text)
    if (c == ',' || c == ';' || c == '[' || c == ']') c = ' ';
  std::istringstream in(text);
  std::string token;
  while (in >> token) {
    try {
      numbers->push_back(std::stoi(token, nullptr, 0));
    } catch (...) {
      return false;
    }
  }
  return true;
}

// "$1E", "0x1E", "1E", 30: a TIA colour; "" or "-": the row above's (-1). -2: not a colour.
int colourOf(const std::string& written) {
  std::string text = written;
  while (!text.empty() && (text.front() == ' ' || text.front() == '#')) text.erase(text.begin());
  while (!text.empty() && text.back() == ' ') text.pop_back();
  if (text.empty() || text == "-" || text == "." || lowered(text) == "same") return -1;
  int base = 10;
  if (text[0] == '$') text = text.substr(1), base = 16;
  else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text = text.substr(2), base = 16;
  else if (text.find_first_not_of("0123456789") != std::string::npos) base = 16;
  if (text.empty() || text.find_first_not_of(base == 16 ? "0123456789abcdefABCDEF" : "0123456789") != std::string::npos) return -2;
  int value = (int)std::strtol(text.c_str(), nullptr, base);
  return value >= 0 && value <= 255 ? (value & 0xFE) : -2;
}

// The file a tool works on: its path in the project, its text (as the editor holds it, else from disk) and whether it exists.
struct Target {
  std::string absolute, relative, text;
  bool exists = false;
};

bool openTarget(const ToolEnv& env, const json& args, const char* extension, Target* target, std::string* error) {
  std::string written = textOf(args, "path");
  if (written.empty()) {
    *error = std::string("a \"path\" is needed (a ") + extension + " file of the project)";
    return false;
  }
  if (!endsWith(lowered(written), extension)) {
    *error = std::string("the path must end in ") + extension + ": " + written;
    return false;
  }
  if (!resolveInProject(env.projectDir, written, &target->absolute, error)) return false;
  std::error_code code;
  target->relative = fs::relative(target->absolute, env.projectDir, code).generic_string();
  if (code || target->relative.empty()) target->relative = written;
  if (env.openFileText && env.openFileText(target->relative, &target->text)) {
    target->exists = true;
    return true;
  }
  if (fs::is_regular_file(target->absolute, code)) {
    std::ifstream in(target->absolute, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    target->text = buffer.str();
    target->exists = true;
  }
  return true;
}

ToolResult propose(const Target& target, const std::string& after, const std::string& preview, const std::string& note) {
  ToolResult result;
  result.proposed = true;
  result.edit.path = target.absolute, result.edit.relative = target.relative;
  result.edit.before = target.text, result.edit.after = after;
  result.edit.created = !target.exists;
  result.edit.changes = 1;
  result.edit.preview = preview, result.edit.note = note;
  result.text = "The change to " + target.relative + " was shown to the user and waits for their decision.\n" + preview;
  return result;
}

// ---- graphics ---------------------------------------------------------------------------------------------------------

gfx::Profile profileOf(const ToolEnv& env, const gfx::Asset& asset) {
  gfx::Profile profile;
  if (env.grounding.batariBasic) profile.kernel = gfx::Kernel::Standard;
  if (!asset.kernel.empty()) gfx::kernelFromId(asset.kernel, &profile.kernel);
  return profile;
}

std::string issueLines(const std::vector<gfx::Issue>& issues) {
  std::string out;
  for (auto& issue : issues) out += std::string(issue.severity == gfx::Issue::Error ? "error: " : issue.severity == gfx::Issue::Warning ? "warning: " : "note: ") + issue.text + "\n";
  return out;
}

bool loadAsset(const Target& target, gfx::Asset* asset, std::string* error) {
  if (!target.exists || target.text.find_first_not_of(" \t\r\n") == std::string::npos) return true;  // a new file: empty
  std::string why;
  if (!gfx::fromJson(target.text, asset, &why)) {
    *error = "the file does not load: " + why;
    return false;
  }
  return true;
}

bool objectOf(const std::string& written, gfx::Obj* object) {
  std::string name = lowered(written);
  for (auto& c : name)
    if (c == ' ' || c == '_') c = 0;
  name.erase(std::remove(name.begin(), name.end(), 0), name.end());
  if (name == "player0") name = "p0";
  if (name == "player1") name = "p1";
  if (name == "missile0") name = "m0";
  if (name == "missile1") name = "m1";
  if (name == "ball") name = "bl";
  for (int index = 0; index < gfx::kObjects; index++)
    if (lowered(gfx::objName((gfx::Obj)index)) == name) {
      *object = (gfx::Obj)index;
      return true;
    }
  return false;
}

// One row of a drawing: true per pixel. '.', ' ', '0', '-', '_' are off; anything else that is not a separator is on.
bool pixelsOf(const std::string& row, std::vector<bool>* pixels, std::string* error) {
  pixels->clear();
  for (char c : row) {
    if (c == '.' || c == ' ' || c == '0' || c == '-' || c == '_') pixels->push_back(false);
    else if (c == '#' || c == 'X' || c == 'x' || c == '1' || c == '*' || c == '@' || c == 'O' || c == 'o') pixels->push_back(true);
    else if (c == '|' || c == '\t') continue;
    else {
      *error = std::string("a row may only have X (or #) for a lit pixel and . for an empty one; found '") + c + "' in \"" + row + "\"";
      return false;
    }
  }
  return true;
}

std::string drawLayer(const gfx::Layer& layer, int frame) {
  std::string out;
  for (int row = 0; row < layer.height && row < 64; row++) {
    std::string line;
    for (int x = 0; x < layer.width(); x++) line += layer.pixel(frame, row, x) ? 'X' : '.';
    out += "  " + line + " " + gfx::hexColour(layer.colour(frame, row)) + "\n";
  }
  return out;
}

ToolResult gfxSetSprite(const ToolEnv& env, const json& args) {
  Target target;
  std::string error;
  if (!openTarget(env, args, ".vcsgfx", &target, &error)) return fail(error);
  gfx::Asset asset;
  if (!loadAsset(target, &asset, &error)) return fail(error);
  std::vector<std::string> rows = linesOf(args, "rows");
  while (!rows.empty() && rows.back().find_first_not_of(" \t") == std::string::npos) rows.pop_back();
  if (rows.empty()) return fail("gfx_set_sprite needs \"rows\": the drawing, one text per row, top first, X for a lit pixel and . for an empty one (8 per row for a player)");
  if (rows.size() > 192) return fail("a sprite cannot be taller than the screen: " + std::to_string(rows.size()) + " rows");
  gfx::Obj object = gfx::Obj::P0;
  std::string objectText = textOf(args, "object", "P0");
  if (!objectOf(objectText, &object)) return fail("\"object\" must be one of the TIA's objects: P0, P1 (players, 8 pixels wide), M0, M1 (missiles), BL (ball); got \"" + objectText + "\"");
  std::string name = textOf(args, "sprite", fs::path(target.relative).stem().string());
  int frame = std::max(0, numberOf(args, "frame", 0));
  if (frame > 63) return fail("frame " + std::to_string(frame) + " is too far: 64 animation frames at most");
  int size = numberOf(args, "size", 0);
  if (size != 0 && size != 1 && size != 2 && size != 4 && !(size == 8 && !gfx::isPlayer(object))) return fail("\"size\" is 1, 2 or 4 (a player's stretch) or 1, 2, 4 or 8 (a missile's or the ball's width)");
  // the pixels
  int widest = gfx::isPlayer(object) ? 8 : 1;
  std::vector<std::vector<bool>> drawing;
  for (auto& row : rows) {
    std::vector<bool> pixels;
    if (!pixelsOf(row, &pixels, &error)) return fail(error);
    while (!pixels.empty() && !pixels.back() && (int)pixels.size() > widest) pixels.pop_back();  // empty pixels past the width do no harm
    if ((int)pixels.size() > widest)
      return fail(gfx::isPlayer(object) ? "a player is 8 pixels wide and the row \"" + row + "\" has " + std::to_string(pixels.size()) +
                                              ". Draw it in 8 columns, or split a wider figure in two layers: object P0 for the left 8 columns and object P1 with \"dx\": 8 for the right ones"
                                        : "a missile or the ball has one column per row (X or .): its width is \"size\" (1, 2, 4 or 8)");
    drawing.push_back(pixels);
  }
  // the colours: one for all, or one per row
  std::vector<int> colours(rows.size(), -1);
  std::vector<std::string> colourTexts = linesOf(args, "colours");
  if (colourTexts.size() == 1 && rows.size() > 1 && colourTexts[0].find_first_of(" ,") != std::string::npos) {  // "$1E $2A $2A" in one text
    std::string all = colourTexts[0];
    for (auto& c : all)
      if (c == ',') c = ' ';
    std::istringstream in(all);
    colourTexts.clear();
    for (std::string token; in >> token;) colourTexts.push_back(token);
  }
  if (colourTexts.size() > rows.size()) return fail("there are " + std::to_string(colourTexts.size()) + " colours for " + std::to_string(rows.size()) + " rows: one colour per row at most");
  for (size_t i = 0; i < colourTexts.size(); i++) {
    colours[i] = colourOf(colourTexts[i]);
    if (colours[i] == -2) return fail("\"" + colourTexts[i] + "\" is not a TIA colour: write it as $00 to $FE (hue in the high digit, brightness in the low one, even values)");
  }
  if (has(args, "colour")) {
    int all = colourOf(textOf(args, "colour"));
    if (all < 0) return fail("\"colour\" is not a TIA colour: write it as $00 to $FE");
    if (colourTexts.empty()) colours[0] = all;
  }
  // the sprite and the layer it goes to
  gfx::Sprite* sprite = nullptr;
  for (auto& item : asset.sprites)
    if (lowered(item.name) == lowered(name)) sprite = &item;
  bool newSprite = !sprite;
  if (!sprite) {
    asset.sprites.emplace_back();
    sprite = &asset.sprites.back();
    sprite->name = name;
  }
  if (frame >= sprite->frames) sprite->setFrames(frame + 1);
  gfx::Layer* layer = nullptr;
  for (auto& item : sprite->layers)
    if (item.obj == object) layer = &item;
  if (!layer) {
    sprite->layers.emplace_back();
    layer = &sprite->layers.back();
    layer->obj = object;
    layer->size = 1;
    layer->resize(sprite->frames, (int)rows.size());
  }
  if (size) layer->size = size;
  if (has(args, "dx")) layer->dx = std::clamp(numberOf(args, "dx", 0), -160, 160);
  if (has(args, "dy")) layer->dy = std::clamp(numberOf(args, "dy", 0), -192, 192);
  if (has(args, "layer_name")) layer->name = textOf(args, "layer_name");
  if (has(args, "line_height")) sprite->lineHeight = std::clamp(numberOf(args, "line_height", 2), 1, 16);
  int oldHeight = layer->height;
  layer->resize(sprite->frames, (int)rows.size());
  for (size_t row = 0; row < rows.size(); row++) {
    uint8_t bits = 0;
    if (gfx::isPlayer(object)) {
      for (size_t x = 0; x < drawing[row].size() && x < 8; x++)
        if (drawing[row][x]) bits |= (uint8_t)(0x80 >> x);
    } else {
      bits = std::any_of(drawing[row].begin(), drawing[row].end(), [](bool on) { return on; }) ? 1 : 0;
    }
    layer->bits[frame][row] = bits;
    layer->colours[frame][row] = colours[row];
  }
  gfx::Profile profile = profileOf(env, asset);
  std::string issues = issueLines(gfx::check(*sprite, profile));
  std::string preview = std::string(newSprite ? "New sprite \"" : "Sprite \"") + sprite->name + "\", " + gfx::objName(object) + ", frame " + std::to_string(frame) + " of " + std::to_string(sprite->frames) + ", " +
                        std::to_string(rows.size()) + " rows (X = pixel, then the row's colour):\n" + drawLayer(*layer, frame);
  std::string note;
  if (!newSprite && oldHeight != (int)rows.size() && sprite->frames > 1) note += "The layer's height changed from " + std::to_string(oldHeight) + " to " + std::to_string(rows.size()) + " rows in every frame.\n";
  if (!issues.empty()) note += "For " + profile.describe() + ":\n" + issues;
  return propose(target, gfx::toJson(asset), preview + note, note);
}

ToolResult gfxSetPlayfield(const ToolEnv& env, const json& args) {
  Target target;
  std::string error;
  if (!openTarget(env, args, ".vcsgfx", &target, &error)) return fail(error);
  gfx::Asset asset;
  if (!loadAsset(target, &asset, &error)) return fail(error);
  std::vector<std::string> rows = linesOf(args, "rows");
  while (!rows.empty() && rows.back().find_first_not_of(" \t") == std::string::npos) rows.pop_back();
  if (rows.empty()) return fail("gfx_set_playfield needs \"rows\": one text per row, top first, X for a block and . for none; 20 characters for the left half (the right half mirrors or repeats it) or 40 for the whole line");
  if (rows.size() > 192) return fail("a playfield cannot have more rows than the screen has lines: " + std::to_string(rows.size()));
  std::string name = textOf(args, "playfield", fs::path(target.relative).stem().string());
  bool mirror = flagOf(args, "mirror", true);
  gfx::Playfield* playfield = nullptr;
  for (auto& item : asset.playfields)
    if (lowered(item.name) == lowered(name)) playfield = &item;
  bool created = !playfield;
  if (!playfield) {
    asset.playfields.emplace_back();
    playfield = &asset.playfields.back();
    playfield->name = name;
  }
  if (has(args, "line_height")) playfield->lineHeight = std::clamp(numberOf(args, "line_height", 8), 1, 192);
  else if (created) playfield->lineHeight = std::max(1, 192 / (int)rows.size());
  if (has(args, "colour")) {
    int colour = colourOf(textOf(args, "colour"));
    if (colour < 0) return fail("\"colour\" is not a TIA colour: write it as $00 to $FE");
    playfield->pfColour = colour;
  }
  if (has(args, "background")) {
    int colour = colourOf(textOf(args, "background"));
    if (colour < 0) return fail("\"background\" is not a TIA colour: write it as $00 to $FE");
    playfield->bkColour = colour;
  }
  std::vector<std::string> rowColours = linesOf(args, "row_colours");
  if (rowColours.size() > rows.size()) return fail("there are more row colours than rows");
  playfield->rows.assign(rows.size(), gfx::PfRow());
  int asymmetric = 0;
  for (size_t row = 0; row < rows.size(); row++) {
    std::vector<bool> pixels;
    if (!pixelsOf(rows[row], &pixels, &error)) return fail(error);
    while (pixels.size() > 40 && !pixels.back()) pixels.pop_back();
    if (pixels.size() > 40) return fail("the playfield is 40 blocks wide and the row \"" + rows[row] + "\" has " + std::to_string(pixels.size()));
    bool whole = pixels.size() > 20;
    pixels.resize(whole ? 40 : 20, false);
    gfx::PfRow& pfRow = playfield->rows[row];
    pfRow.mode = mirror ? gfx::PfMode::Mirror : gfx::PfMode::Repeat;
    if (whole) {  // a whole line that is really a mirrored or repeated half stays symmetric: the kernel has less to do
      bool mirrored = true, repeated = true;
      for (int column = 0; column < 20; column++) {
        mirrored = mirrored && pixels[20 + column] == pixels[19 - column];
        repeated = repeated && pixels[20 + column] == pixels[column];
      }
      if (mirrored) pfRow.mode = gfx::PfMode::Mirror, whole = false;
      else if (repeated) pfRow.mode = gfx::PfMode::Repeat, whole = false;
    }
    for (int column = 0; column < (whole ? 40 : 20); column++) playfield->setPixel((int)row, column, pixels[column]);
    asymmetric += whole;
    if (row < rowColours.size()) {
      int colour = colourOf(rowColours[row]);
      if (colour == -2) return fail("\"" + rowColours[row] + "\" is not a TIA colour");
      pfRow.pf = colour;
    }
  }
  gfx::Profile profile = profileOf(env, asset);
  std::string issues = issueLines(gfx::check(*playfield, profile));
  std::string preview = std::string(created ? "New playfield \"" : "Playfield \"") + playfield->name + "\": " + std::to_string(rows.size()) + " rows of " + std::to_string(playfield->lineHeight) +
                        " scanlines, colour " + gfx::hexColour(playfield->pfColour) + " on " + gfx::hexColour(playfield->bkColour) + " (as the screen shows it, 40 blocks):\n";
  for (size_t row = 0; row < playfield->rows.size() && row < 64; row++) {
    std::string line;
    for (int column = 0; column < 40; column++) line += playfield->pixel((int)row, column) ? 'X' : '.';
    preview += "  " + line + "\n";
  }
  std::string note;
  if (asymmetric) note += std::to_string(asymmetric) + " row(s) have two different halves: the kernel must rewrite PF0-PF2 in the middle of those lines.\n";
  if (!issues.empty()) note += "For " + profile.describe() + ":\n" + issues;
  return propose(target, gfx::toJson(asset), preview + note, note);
}

ToolResult gfxSetAnimation(const ToolEnv& env, const json& args) {
  Target target;
  std::string error;
  if (!openTarget(env, args, ".vcsgfx", &target, &error)) return fail(error);
  if (!target.exists) return fail("no such file: " + target.relative + " (draw the sprite's frames first with gfx_set_sprite)");
  gfx::Asset asset;
  if (!loadAsset(target, &asset, &error)) return fail(error);
  std::string name = textOf(args, "sprite");
  gfx::Sprite* sprite = nullptr;
  for (auto& item : asset.sprites)
    if (name.empty() ? asset.sprites.size() == 1 : lowered(item.name) == lowered(name)) sprite = &item;
  if (!sprite) {
    std::string known;
    for (auto& item : asset.sprites) known += (known.empty() ? "" : ", ") + item.name;
    return fail("\"sprite\" must name a sprite of the file: " + (known.empty() ? std::string("it has none") : known));
  }
  std::vector<int> frames;
  if (!numbersOf(args, "frames", &frames) || frames.empty()) return fail("gfx_set_animation needs \"frames\": the frames in the order they are shown, for example [0, 1, 2, 1]");
  for (int frame : frames)
    if (frame < 0 || frame >= sprite->frames) return fail("the sprite has frames 0 to " + std::to_string(sprite->frames - 1) + "; frame " + std::to_string(frame) + " is not one (draw it first with gfx_set_sprite)");
  int ticks = std::clamp(numberOf(args, "ticks", 8), 1, 255);
  std::string animationName = textOf(args, "name", "anim");
  gfx::Animation* animation = nullptr;
  for (auto& item : sprite->animations)
    if (lowered(item.name) == lowered(animationName)) animation = &item;
  if (!animation) {
    sprite->animations.emplace_back();
    animation = &sprite->animations.back();
    animation->name = animationName;
  }
  animation->steps.clear();
  for (int frame : frames) animation->steps.push_back({frame, ticks});
  animation->loop = flagOf(args, "loop", true);
  std::string preview = "Animation \"" + animation->name + "\" of sprite \"" + sprite->name + "\": frames";
  for (int frame : frames) preview += " " + std::to_string(frame);
  preview += ", " + std::to_string(ticks) + " video frames each (" + std::to_string(animation->length()) + " in all), " + (animation->loop ? "looping" : "played once") + ".\n";
  return propose(target, gfx::toJson(asset), preview, "");
}

std::string capped(const std::string& text, size_t limit) {
  if (text.size() <= limit) return text;
  size_t cut = text.rfind('\n', limit);
  return text.substr(0, cut == std::string::npos ? limit : cut) + "\n[" + std::to_string(text.size() - limit) + " more characters not shown; ask for one item with \"item\"]\n";
}

ToolResult gfxCode(const ToolEnv& env, const json& args) {
  Target target;
  std::string error;
  if (!openTarget(env, args, ".vcsgfx", &target, &error)) return fail(error);
  if (!target.exists) return fail("no such file: " + target.relative);
  gfx::Asset asset;
  if (!loadAsset(target, &asset, &error)) return fail(error);
  gfx::Profile profile = profileOf(env, asset);
  std::string format = lowered(textOf(args, "format", profile.basic() ? "bb" : "dasm"));
  if (format == "asm" || format == "assembly") format = "dasm";
  if (format == "basic" || format == "batari" || format == "bas") format = "bb";
  if (format != "dasm" && format != "bb" && format != "c") return fail("\"format\" is dasm, bb (batari Basic) or c");
  std::string wanted = lowered(textOf(args, "item"));
  std::string out;
  int shown = 0;
  for (auto& sprite : asset.sprites) {
    if (!wanted.empty() && lowered(sprite.name) != wanted) continue;
    out += format == "dasm" ? gfx::exportDasm(sprite) : format == "c" ? gfx::exportC(sprite) : gfx::exportBasic(sprite, profile.basic() ? profile : [] { gfx::Profile basic; basic.kernel = gfx::Kernel::Standard; return basic; }());
    out += "\n";
    shown++;
  }
  for (auto& playfield : asset.playfields) {
    if (!wanted.empty() && lowered(playfield.name) != wanted) continue;
    out += format == "dasm" ? gfx::exportDasm(playfield) : format == "c" ? gfx::exportC(playfield) : gfx::exportBasic(playfield, profile.basic() ? profile : [] { gfx::Profile basic; basic.kernel = gfx::Kernel::Standard; return basic; }());
    out += "\n";
    shown++;
  }
  if (!shown) return fail(wanted.empty() ? "the file has no sprites or playfields" : "the file has no sprite or playfield called \"" + wanted + "\"");
  std::string stem = fs::path(target.relative).stem().string();
  std::string header = format == "bb" ? "batari Basic blocks for " + target.relative + " (paste them in the program, or let the editor keep a `rem vcsgfx` region up to date):\n"
                                      : "The tables of " + target.relative + " (the graphics editor writes them to " + stem + (format == "c" ? ".gfx.h" : ".gfx.asm") +
                                            " every time the file is saved; the program includes that file and uses these labels):\n";
  return ok(header + capped(out, env.readCapChars));
}

// ---- music ------------------------------------------------------------------------------------------------------------

const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string noteText(int midi) {
  if (midi == music::kNoNote) return ".";
  if (midi == music::kNoteOff) return "off";
  return std::string(kNoteNames[((midi % 12) + 12) % 12]) + std::to_string(midi / 12 - 1);
}

bool loadSong(const Target& target, music::Song* song, std::string* error) {
  if (!target.exists || target.text.find_first_not_of(" \t\r\n") == std::string::npos) {
    *song = music::blankSong();
    song->name = fs::path(target.relative).stem().string();
    return true;
  }
  std::string why;
  if (!music::fromJson(target.text, song, &why)) {
    *error = "the file does not load: " + why;
    return false;
  }
  if (song->instruments.empty()) song->instruments = music::blankSong().instruments;
  return true;
}

std::string instrumentList(const music::Song& song) {
  std::string out;
  for (size_t i = 0; i < song.instruments.size(); i++) out += (i ? ", " : "") + std::to_string(i) + " = " + song.instruments[i].name + (song.instruments[i].drum ? " (drum)" : "");
  return out;
}

// The melodic notes the TIA cannot play in tune with their instrument: the TIA has 32 pitches per waveform, so many notes
// are off. Said so the model (or the user) can move the tune to another octave or key, or allow another waveform.
std::string tuningNotes(const ToolEnv& env, const music::Song& song) {
  if (song.samples()) return "";
  double clock = music::audioClockHz(env.grounding.tv.empty() ? "NTSC" : env.grounding.tv);
  std::set<std::pair<int, int>> seen;
  std::string out;
  int listed = 0;
  for (auto& pattern : song.patterns)
    for (int channel = 0; channel < song.channelCount(); channel++) {
      int instrument = 0;
      for (auto& cell : pattern.cells[song.trackCells(channel)]) {
        if (cell.instrument >= 0) instrument = cell.instrument;
        if (cell.note < 0 || instrument >= (int)song.instruments.size() || song.instruments[instrument].drum) continue;
        if (!seen.insert({cell.note, instrument}).second) continue;
        music::NotePitch pitch = song.pitchOf(cell.note, song.instruments[instrument], clock);
        if (pitch.playable && std::fabs(pitch.cents) <= 30) continue;
        if (++listed > 8) continue;
        out += "  " + noteText(cell.note) + " with instrument " + std::to_string(instrument) + ": " +
               (pitch.playable ? std::to_string((int)std::lround(std::fabs(pitch.cents))) + " cents " + (pitch.cents > 0 ? "sharp" : "flat") : std::string("no TIA pitch within half a semitone")) + "\n";
      }
    }
  if (out.empty()) return "";
  return "Notes the TIA plays out of tune (it has only 32 pitches per waveform; another octave, a transposition of the song or another AUDC in the instrument's \"controls\" may fix them):\n" + out +
         (listed > 8 ? "  ... and " + std::to_string(listed - 8) + " more\n" : "");
}

ToolResult musicSetSong(const ToolEnv& env, const json& args) {
  Target target;
  std::string error;
  if (!openTarget(env, args, ".vcsmusic", &target, &error)) return fail(error);
  music::Song song;
  if (!loadSong(target, &song, &error)) return fail(error);
  if (has(args, "name")) song.name = textOf(args, "name");
  if (has(args, "speed")) song.speed = std::clamp(numberOf(args, "speed", 6), 1, 64);
  if (has(args, "swing")) song.swing = std::clamp(numberOf(args, "swing", 0), 0, 64);
  if (has(args, "transpose")) song.tuning.transpose = std::clamp(numberOf(args, "transpose", 0), -48, 48);
  if (has(args, "order")) {
    std::vector<int> order;
    if (!numbersOf(args, "order", &order) || order.empty()) return fail("\"order\" is the list of pattern numbers in playing order, for example [0, 0, 1, 0]");
    for (int entry : order)
      if (entry < 0 || entry >= (int)song.patterns.size()) return fail("the song has patterns 0 to " + std::to_string((int)song.patterns.size() - 1) + "; " + std::to_string(entry) + " is not one (write it first with music_set_pattern)");
    song.order = order;
  }
  if (has(args, "loop_to")) song.loopTo = std::clamp(numberOf(args, "loop_to", 0), -1, std::max(0, (int)song.order.size() - 1));
  if (has(args, "export_to")) {
    std::string where = lowered(textOf(args, "export_to"));
    if (where == "none") where.clear();
    if (where == "asm") where = "dasm";
    if (!where.empty() && where != "dasm" && where != "bb" && where != "c" && where != "dpc" && where != "cdfj") return fail("\"export_to\" is dasm, bb (batari Basic), c, dpc, cdfj or none");
    song.exportTo = where;
  }
  song.loopTo = std::min(song.loopTo, (int)song.order.size() - 1);
  std::string preview = "Song \"" + song.name + "\": speed " + std::to_string(song.speed) + " video frames per row" + (song.swing ? " (odd rows " + std::to_string(song.swing) + ")" : "") + ", order";
  for (int entry : song.order) preview += " " + std::to_string(entry);
  preview += song.loopTo < 0 ? ", played once" : ", then back to entry " + std::to_string(song.loopTo);
  preview += song.exportTo.empty() ? ". Not exported yet: set \"export_to\" for the program to use it.\n" : ", exported as " + song.exportTo + " on every save.\n";
  return propose(target, music::toJson(song), preview, "");
}

ToolResult musicSetInstrument(const ToolEnv& env, const json& args) {
  Target target;
  std::string error;
  if (!openTarget(env, args, ".vcsmusic", &target, &error)) return fail(error);
  music::Song song;
  if (!loadSong(target, &song, &error)) return fail(error);
  int index = numberOf(args, "index", (int)song.instruments.size());
  if (index < 0 || index > (int)song.instruments.size() || index > 15) return fail("\"index\" is 0 to " + std::to_string(std::min(15, (int)song.instruments.size())) + " (the last one adds an instrument; 16 at most). The song has: " + instrumentList(song));
  bool added = index == (int)song.instruments.size();
  if (added) song.instruments.emplace_back();
  music::Instrument& instrument = song.instruments[index];
  if (has(args, "name")) instrument.name = textOf(args, "name");
  if (has(args, "kind")) {
    std::string kind = lowered(textOf(args, "kind"));
    if (kind != "melodic" && kind != "drum") return fail("\"kind\" is melodic (its notes have a pitch) or drum (fixed AUDC and AUDF values frame by frame)");
    instrument.drum = kind == "drum";
  }
  std::vector<int> numbers;
  auto inRange = [&](const char* what, int low, int high) -> std::string {
    for (int value : numbers)
      if (value < low || value > high) return std::string("\"") + what + "\" takes values " + std::to_string(low) + " to " + std::to_string(high) + "; got " + std::to_string(value);
    return "";
  };
  if (has(args, "volume")) {
    if (!numbersOf(args, "volume", &numbers) || numbers.empty()) return fail("\"volume\" is the loudness frame by frame, 0 to 15, for example [15, 12, 9, 6, 4]");
    if (!(error = inRange("volume", 0, 15)).empty()) return fail(error);
    if (numbers.size() > 64) return fail("a volume envelope has 64 frames at most");
    if (instrument.volumes.empty()) instrument.volumes.emplace_back();
    instrument.volumes[0].values = numbers;
    instrument.volumes[0].loop = -1;
  }
  if (has(args, "volume_loop")) {
    if (instrument.volumes.empty()) instrument.volumes.emplace_back();
    instrument.volumes[0].loop = std::clamp(numberOf(args, "volume_loop", -1), -1, std::max(0, (int)instrument.volumes[0].values.size() - 1));
  }
  if (has(args, "release")) {
    if (!numbersOf(args, "release", &numbers)) return fail("\"release\" is the loudness frame by frame after the note is released, 0 to 15 (an empty list: silent at once)");
    if (!(error = inRange("release", 0, 15)).empty()) return fail(error);
    instrument.releases = {numbers};
  }
  if (has(args, "controls")) {
    if (!numbersOf(args, "controls", &numbers) || numbers.empty()) return fail("\"controls\" is the list of AUDC waveforms a melodic instrument may use, for example [4, 12] (pure tones) or [1] (buzz), [6] or [7] (bass)");
    if (!(error = inRange("controls", 1, 15)).empty()) return fail(error);
    instrument.controls = numbers;
  }
  if (has(args, "pitch")) {
    if (!numbersOf(args, "pitch", &numbers)) return fail("\"pitch\" is cents added to the note frame by frame, for example [0, 0, 20, 0, -20] for a vibrato");
    if (!(error = inRange("pitch", -2400, 2400)).empty()) return fail(error);
    instrument.pitch = numbers, instrument.pitchUnit = music::PitchUnit::Cents;
  }
  if (has(args, "drum_control")) {
    if (!numbersOf(args, "drum_control", &numbers) || numbers.empty()) return fail("\"drum_control\" is AUDC frame by frame, 0 to 15 (8 is noise)");
    if (!(error = inRange("drum_control", 0, 15)).empty()) return fail(error);
    instrument.drumControl = numbers;
  }
  if (has(args, "drum_frequency")) {
    if (!numbersOf(args, "drum_frequency", &numbers) || numbers.empty()) return fail("\"drum_frequency\" is AUDF frame by frame, 0 to 31 (higher is lower in pitch)");
    if (!(error = inRange("drum_frequency", 0, 31)).empty()) return fail(error);
    instrument.drumFrequency = numbers;
  }
  auto list = [](const std::vector<int>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); i++) out += (i ? " " : "") + std::to_string(values[i]);
    return out.empty() ? std::string("(none)") : out;
  };
  std::string preview = std::string(added ? "New instrument " : "Instrument ") + std::to_string(index) + " \"" + instrument.name + "\", " + (instrument.drum ? "drum" : "melodic") + ":\n  volume " +
                        list(instrument.volumes.empty() ? std::vector<int>() : instrument.volumes[0].values) + "\n  release " + list(instrument.releases.empty() ? std::vector<int>() : instrument.releases[0]) + "\n";
  if (instrument.drum) preview += "  AUDC " + list(instrument.drumControl) + "\n  AUDF " + list(instrument.drumFrequency) + "\n";
  else preview += "  waveforms (AUDC) " + list(instrument.controls) + (instrument.pitch.empty() ? "" : "\n  pitch (cents) " + list(instrument.pitch)) + "\n";
  std::string note = tuningNotes(env, song);
  return propose(target, music::toJson(song), preview + note, note);
}

// One cell written by a model: "C4", "F#3:1" (instrument 1), "C4:0:V2" (effect), "." (nothing), "off" (released).
bool cellOf(const std::string& token, int defaultInstrument, music::Cell* cell, std::string* error) {
  *cell = music::Cell();
  std::string text = token;
  std::vector<std::string> parts;
  for (size_t start = 0; start <= text.size();) {
    size_t colon = text.find(':', start);
    if (colon == std::string::npos) colon = text.size();
    parts.push_back(text.substr(start, colon - start));
    start = colon + 1;
  }
  std::string note = lowered(parts[0]);
  if (note.empty() || note.find_first_not_of(".-_") == std::string::npos) return true;  // nothing here
  if (note == "off" || note == "=" || note == "===" || note == "x" || note == "rest") {
    cell->note = music::kNoteOff;
    return true;
  }
  int midi = 0;
  if (!parseNoteName(parts[0], &midi)) {
    *error = "\"" + token + "\" is not a note: write C4, F#3 or Bb2 (C4 is middle C), . for nothing and off to release the note";
    return false;
  }
  cell->note = midi;
  cell->instrument = defaultInstrument;
  if (parts.size() > 1 && !parts[1].empty()) {
    try {
      cell->instrument = std::stoi(parts[1]);
    } catch (...) {
      *error = "in \"" + token + "\" the part after the colon is the instrument's number";
      return false;
    }
  }
  if (parts.size() > 2 && parts[2].size() >= 2) {
    char effect = (char)std::toupper((unsigned char)parts[2][0]);
    if (effect != music::kEffectVolume && effect != music::kEffectRelease) {
      *error = "in \"" + token + "\" the effect is V (volume envelope) or R (release) and a number";
      return false;
    }
    cell->effect = effect;
    cell->effectValue = std::clamp(std::atoi(parts[2].c_str() + 1), 0, 15);
  }
  return true;
}

ToolResult musicSetPattern(const ToolEnv& env, const json& args) {
  Target target;
  std::string error;
  if (!openTarget(env, args, ".vcsmusic", &target, &error)) return fail(error);
  music::Song song;
  if (!loadSong(target, &song, &error)) return fail(error);
  int index = numberOf(args, "pattern", 0);
  if (index < 0 || index > (int)song.patterns.size() || index > 63) return fail("\"pattern\" is 0 to " + std::to_string(std::min(63, (int)song.patterns.size())) + " (the last one adds a pattern)");
  int channel = numberOf(args, "channel", 0);
  if (channel < 0 || channel >= song.channelCount()) return fail("\"channel\" is 0 to " + std::to_string(song.channelCount() - 1) + " (the TIA has two voices)");
  // the notes: a text of tokens, or a list of them
  std::vector<std::string> tokens;
  for (auto& line : linesOf(args, "notes")) {
    std::string text = line;
    for (auto& c : text)
      if (c == ',' || c == '|' || c == ';') c = ' ';
    std::istringstream in(text);
    for (std::string token; in >> token;) tokens.push_back(token);
  }
  if (tokens.empty()) return fail("music_set_pattern needs \"notes\": one token per row, separated by spaces: a note (C4, F#3, Bb2; C4 is middle C), . for nothing, off to release. For example \"C4 . E4 . G4 . off .\"");
  if (tokens.size() > 256) return fail("a pattern has 256 rows at most");
  int defaultInstrument = numberOf(args, "instrument", 0);
  std::vector<music::Cell> cells;
  for (auto& token : tokens) {
    music::Cell cell;
    if (!cellOf(token, defaultInstrument, &cell, &error)) return fail(error);
    if (cell.note >= 0 && (cell.instrument < 0 || cell.instrument >= (int)song.instruments.size()))
      return fail("instrument " + std::to_string(cell.instrument) + " does not exist. The song has: " + instrumentList(song) + " (add one with music_set_instrument)");
    if (cell.note >= 0 && (cell.note < 12 || cell.note > 119)) return fail("\"" + token + "\" is outside what the TIA can play (octaves 0 to 8)");
    cells.push_back(cell);
  }
  bool added = index == (int)song.patterns.size();
  if (added) {
    song.patterns.emplace_back();
    song.patterns.back().resize(std::max(16, (int)cells.size()));
    if (std::find(song.order.begin(), song.order.end(), index) == song.order.end()) song.order.push_back(index);  // a new pattern is played after the others
  }
  music::Pattern& pattern = song.patterns[index];
  if (has(args, "name")) pattern.name = textOf(args, "name");
  int rows = has(args, "rows") ? std::clamp(numberOf(args, "rows", pattern.rows), 1, 256) : std::max(pattern.rows, (int)cells.size());
  if (rows < (int)cells.size()) return fail("there are " + std::to_string(cells.size()) + " notes for " + std::to_string(rows) + " rows");
  pattern.resize(rows);
  auto& track = pattern.cells[song.trackCells(channel)];
  track.assign(rows, music::Cell());  // the channel is rewritten whole: what is not in "notes" is silence
  for (size_t row = 0; row < cells.size(); row++) track[row] = cells[row];
  // as a tracker shows it
  std::string preview = std::string(added ? "New pattern " : "Pattern ") + std::to_string(index) + (pattern.name.empty() ? "" : " \"" + pattern.name + "\"") + ", " + std::to_string(rows) + " rows, " +
                        std::to_string(song.rowFrames(pattern, 0)) + " video frames per row (row: channel 0 | channel 1):\n";
  for (int row = 0; row < rows && row < 64; row++) {
    std::string line = (row < 10 ? "  0" : "  ") + std::to_string(row);
    for (int shown = 0; shown < song.channelCount(); shown++) {
      const music::Cell& cell = pattern.cells[song.trackCells(shown)][row];
      std::string text = noteText(cell.note) + (cell.note >= 0 && cell.instrument >= 0 ? ":" + std::to_string(cell.instrument) : "");
      line += " | " + text + std::string(text.size() < 7 ? 7 - text.size() : 0, ' ');
    }
    preview += line + "\n";
  }
  if (rows > 64) preview += "  ...\n";
  std::string note = tuningNotes(env, song);
  if (added) note += "The new pattern was added at the end of the order (music_set_song changes the order).\n";
  return propose(target, music::toJson(song), preview + note, note);
}

ToolResult musicSetEffect(const ToolEnv& env, const json& args) {
  Target target;
  std::string error;
  if (!openTarget(env, args, ".vcsmusic", &target, &error)) return fail(error);
  music::Song song;
  if (!loadSong(target, &song, &error)) return fail(error);
  std::string name = textOf(args, "name");
  if (name.empty()) return fail("music_set_effect needs a \"name\" (what happens in the game: jump, shot, pickup...)");
  int instrument = numberOf(args, "instrument", 0);
  if (instrument < 0 || instrument >= (int)song.instruments.size()) return fail("instrument " + std::to_string(instrument) + " does not exist. The song has: " + instrumentList(song) + " (add one with music_set_instrument)");
  int midi = 60;
  std::string noteWritten = textOf(args, "note", "C4");
  if (!parseNoteName(noteWritten, &midi)) return fail("\"note\" is a note name such as C4 or F#5; got \"" + noteWritten + "\"");
  int channel = numberOf(args, "channel", 1);
  if (channel < 0 || channel >= song.channelCount()) return fail("\"channel\" is 0 to " + std::to_string(song.channelCount() - 1));
  music::SoundEffect* effect = nullptr;
  for (auto& item : song.effects)
    if (lowered(item.name) == lowered(name)) effect = &item;
  bool added = !effect;
  if (!effect) {
    song.effects.emplace_back();
    effect = &song.effects.back();
  }
  effect->name = name, effect->instrument = instrument, effect->note = midi, effect->channel = channel;
  std::string preview = std::string(added ? "New sound effect \"" : "Sound effect \"") + name + "\": instrument " + std::to_string(instrument) + " (" + song.instruments[instrument].name + "), note " + noteText(midi) +
                        ", on channel " + std::to_string(channel) + " (the song's notes on that channel wait while it sounds). The program starts it by its symbol " + music::effectSymbol(name) + " (see music_code).\n";
  return propose(target, music::toJson(song), preview, "");
}

ToolResult musicCode(const ToolEnv& env, const json& args) {
  Target target;
  std::string error;
  if (!openTarget(env, args, ".vcsmusic", &target, &error)) return fail(error);
  if (!target.exists) return fail("no such file: " + target.relative);
  music::Song song;
  if (!loadSong(target, &song, &error)) return fail(error);
  std::string format = lowered(textOf(args, "format", song.exportTo.empty() ? (env.grounding.batariBasic ? "bb" : song.samples() ? "c" : "dasm") : song.exportTo));
  if (format == "asm") format = "dasm";
  music::ExportOptions options;
  if (!music::exportTargetOf(format, &options.target)) return fail("\"format\" is dasm, bb (batari Basic), c, dpc or cdfj");
  options.tv = env.grounding.tv.empty() ? "NTSC" : env.grounding.tv;
  if (options.target == music::ExportTarget::BatariBasic) options.ramStart = song.exportRam;
  music::ExportResult exported = music::exportSong(song, music::audioClockHz(options.tv), options);
  std::string out = "The song as the music editor exports it (" + std::to_string(exported.dataBytes) + " bytes of tables plus the player; the editor writes " + music::exportFileName(options.target) +
                    " on every save when the song's export is set, see music_set_song \"export_to\"):\n";
  for (auto& problem : exported.problems) out += "problem: " + problem + "\n";
  return ok(out + capped(exported.text, env.readCapChars));
}

}  // namespace

bool parseNoteName(const std::string& written, int* midi) {
  std::string text;
  for (char c : written)
    if (c != ' ' && c != '-') text += c;
  if (text.empty()) return false;
  if (text.find_first_not_of("0123456789") == std::string::npos) {  // a MIDI number
    *midi = std::atoi(text.c_str());
    return *midi >= 0 && *midi <= 127;
  }
  static const int semitones[] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
  char letter = (char)std::tolower((unsigned char)text[0]);
  if (letter < 'a' || letter > 'g') return false;
  int semitone = semitones[letter - 'a'];
  size_t at = 1;
  if (at < text.size() && (text[at] == '#' || text[at] == 's')) semitone++, at++;
  else if (at < text.size() && text[at] == 'b' && at + 1 < text.size()) semitone--, at++;
  if (at >= text.size() || text.find_first_not_of("0123456789", at) != std::string::npos) return false;
  int octave = std::atoi(text.c_str() + at);
  if (octave > 9) return false;
  *midi = 12 * (octave + 1) + semitone;
  return *midi >= 0 && *midi <= 127;
}

// The tools that draw and compose are offered when the question is about graphics or sound, or asked with a graphics or music
// file in front: they are nine more descriptions in every request, which a question about code does not need.
bool wantsCreativeTools(const std::string& question, const std::string& openFile) {
  std::string file = openFile, text = question;
  for (auto& c : file) c = (char)std::tolower((unsigned char)c);
  for (auto& c : text) c = (char)std::tolower((unsigned char)c);
  auto endsWith = [&](const char* ending) { return file.size() >= strlen(ending) && file.compare(file.size() - strlen(ending), strlen(ending), ending) == 0; };
  if (endsWith(".vcsgfx") || endsWith(".vcsmusic")) return true;
  for (const char* word : {"sprite", "playfield", "gráfic", "grafic", "graphic", "dibuj", "draw", "pixel", "píxel", "animaci", "animation", "vcsgfx", "música", "musica", "music", "song", "canción", "cancion", "melod",
                           "sonido", "sound", "efecto", "sfx", "jingle", "tune", "instrument", "vcsmusic", "fondo", "background", "logo", "tile", "personaje", "character", "enemy", "enemigo", "nave", "ship"})
    if (text.find(word) != std::string::npos) return true;
  return false;
}

bool isCreativeTool(const std::string& name) {
  static const std::set<std::string> names = {"gfx_set_sprite", "gfx_set_playfield", "gfx_set_animation", "gfx_code", "music_set_song", "music_set_instrument", "music_set_pattern", "music_set_effect", "music_code"};
  return names.count(name) > 0;
}

static ToolResult runNamed(const ToolEnv& env, const ToolCall& call);

ToolResult runCreativeTool(const ToolEnv& env, const ToolCall& call) {
  ToolResult result = runNamed(env, call);
  if (result.proposed) result.edit.tool = call.name;
  return result;
}

static ToolResult runNamed(const ToolEnv& env, const ToolCall& call) {
  const std::string& name = call.name;
  if (name == "gfx_set_sprite") return gfxSetSprite(env, call.args);
  if (name == "gfx_set_playfield") return gfxSetPlayfield(env, call.args);
  if (name == "gfx_set_animation") return gfxSetAnimation(env, call.args);
  if (name == "gfx_code") return gfxCode(env, call.args);
  if (name == "music_set_song") return musicSetSong(env, call.args);
  if (name == "music_set_instrument") return musicSetInstrument(env, call.args);
  if (name == "music_set_pattern") return musicSetPattern(env, call.args);
  if (name == "music_set_effect") return musicSetEffect(env, call.args);
  if (name == "music_code") return musicCode(env, call.args);
  return fail("unknown tool: " + name);
}

}  // namespace vcs::assistant
