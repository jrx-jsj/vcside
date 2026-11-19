// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/tool_schema.h"

#include <sstream>

#include <algorithm>
#include <cctype>

namespace vcs::assistant {

using json = nlohmann::json;

namespace {

struct Param {
  const char* name;
  const char* type;  // "string", "integer", "boolean", "array" (search/replace pairs), "strings" and "integers" (lists; one text with lines or numbers will do)
  bool required;
  const char* description;
  std::vector<const char*> aliases;
};
struct Schema {
  const char* tool;
  std::vector<Param> params;
};

const std::vector<const char*> kPath = {"file", "filename", "filepath", "file_path", "name", "location"};
const std::vector<const char*> kQuery = {"text", "q", "search", "term", "keyword", "pattern", "description", "question", "topic", "query_text"};

const std::vector<Schema>& schemas() {
  static const std::vector<Schema> list = {
      {"project_info", {}},
      {"list_files", {{"path", "string", false, "A folder of the project; \".\" is its top.", {"dir", "folder", "directory", "file", "name"}}}},
      {"read_file",
       {{"path", "string", true, "The file (or folder) to read, relative to the project.", kPath},
        {"from", "integer", false, "First line to read (1 by default).", {"start", "start_line", "offset", "line"}},
        {"to", "integer", false, "Last line to read (200 lines at most per call).", {"end", "end_line", "limit_line"}}}},
      {"search", {{"text", "string", true, "Text to find in the project's own files (not case sensitive); a|b|c finds any of several in one call.", {"query", "q", "search", "term", "keyword", "pattern"}}, {"path", "string", false, "Optional: a folder, a file or a pattern like *.asm to look in.", {"folder", "file", "in"}}}},
      {"symbols", {{"name", "string", false, "Part of a label or constant name; all of them if omitted.", {"query", "symbol", "text", "q"}}}},
      {"find_code", {{"query", "string", true, "What the code does, or names in it.", kQuery}}},
      {"kb_lookup", {{"id", "string", true, "The name of a TIA/RIOT register, 6502 instruction, cartridge format or TV standard.", {"name", "register", "instruction", "symbol", "query", "text"}}}},
      {"kb_search", {{"query", "string", true, "A topic in words.", {"text", "q", "search", "topic", "question"}}}},
      {"build_errors", {}},
      {"space_report", {}},
      {"gfx_describe",
       {{"path", "string", true, "The .vcsgfx file.", kPath}, {"frame", "integer", false, "Animation frame to draw (0 by default).", {"index"}}}},
      {"music_describe",
       {{"path", "string", true, "The .vcsmusic file.", kPath}, {"pattern", "integer", false, "Pattern to list (0 by default).", {"index"}}}},
      {"gfx_set_sprite",
       {{"path", "string", true, "The .vcsgfx file (created if missing).", kPath},
        {"sprite", "string", false, "The sprite's name (the file's name by default).", {"item", "sprite_name"}},
        {"rows", "strings", true, "The drawing: one text per row, top first; X is a lit pixel and . an empty one; 8 characters per row for a player.", {"pixels", "lines", "drawing", "bitmap", "data"}},
        {"colours", "strings", false, "One TIA colour per row ($00-$FE); fewer rows than the drawing: the rest keep the colour above.", {"colors", "row_colours", "row_colors", "palette"}},
        {"colour", "string", false, "One TIA colour for the whole drawing ($00-$FE).", {"color"}},
        {"object", "string", false, "The TIA object it is drawn with: P0 (default), P1, M0, M1 or BL.", {"obj", "layer", "player"}},
        {"frame", "integer", false, "The animation frame to draw (0 by default; a new number adds frames).", {"index"}},
        {"size", "integer", false, "A player's stretch (1, 2 or 4) or a missile's or the ball's width (1, 2, 4, 8).", {"scale", "width"}},
        {"dx", "integer", false, "This object's horizontal offset in the sprite, in pixels (for a figure made of two objects).", {"x", "offset_x"}},
        {"dy", "integer", false, "This object's vertical offset in the sprite, in rows.", {"y", "offset_y"}},
        {"line_height", "integer", false, "Scanlines per row (2 for a two-line kernel).", {"lineheight", "scanlines"}},
        {"layer_name", "string", false, "A name for this part of the sprite.", {}}}},
      {"gfx_set_playfield",
       {{"path", "string", true, "The .vcsgfx file (created if missing).", kPath},
        {"playfield", "string", false, "The playfield's name (the file's name by default).", {"item", "playfield_name"}},
        {"rows", "strings", true, "One text per row, top first; X is a block and . none; 20 characters (left half) or 40 (whole line).", {"pixels", "lines", "drawing", "bitmap", "data"}},
        {"mirror", "boolean", false, "For 20-character rows: the right half mirrors the left (true, default) or repeats it (false).", {"mirrored", "reflect"}},
        {"colour", "string", false, "The playfield's TIA colour ($00-$FE).", {"color", "pf", "pf_colour"}},
        {"background", "string", false, "The background's TIA colour.", {"bk", "background_colour", "background_color", "bg"}},
        {"line_height", "integer", false, "Scanlines per row.", {"lineheight", "scanlines", "row_height"}},
        {"row_colours", "strings", false, "A playfield colour per row (needs a kernel that changes COLUPF per row).", {"row_colors", "colours", "colors"}}}},
      {"gfx_set_animation",
       {{"path", "string", true, "The .vcsgfx file.", kPath},
        {"sprite", "string", false, "The sprite's name (needed if the file has several).", {"item"}},
        {"name", "string", false, "The animation's name (\"anim\" by default).", {"animation"}},
        {"frames", "integers", true, "The frames in the order they are shown.", {"steps", "sequence", "order"}},
        {"ticks", "integer", false, "Video frames each one stays (8 by default).", {"speed", "duration"}},
        {"loop", "boolean", false, "It starts again at the end (true by default).", {"looping", "repeat"}}}},
      {"gfx_code",
       {{"path", "string", true, "The .vcsgfx file.", kPath},
        {"format", "string", false, "dasm, bb (batari Basic) or c; the project's by default.", {"language", "target"}},
        {"item", "string", false, "Only this sprite or playfield.", {"sprite", "playfield"}}}},
      {"music_set_song",
       {{"path", "string", true, "The .vcsmusic file (created if missing).", kPath},
        {"name", "string", false, "The song's name.", {"title"}},
        {"speed", "integer", false, "Video frames per row (6 by default; lower is faster).", {"tempo"}},
        {"swing", "integer", false, "Video frames of the odd rows (0: the same as speed).", {}},
        {"transpose", "integer", false, "Semitones added to every note.", {}},
        {"order", "integers", false, "The pattern numbers in playing order.", {"sequence", "patterns"}},
        {"loop_to", "integer", false, "The order entry the song goes back to at the end (-1: it stops).", {"loop"}},
        {"export_to", "string", false, "The export written on every save: dasm, bb, c, dpc, cdfj or none.", {"export", "format", "target"}}}},
      {"music_set_instrument",
       {{"path", "string", true, "The .vcsmusic file (created if missing).", kPath},
        {"index", "integer", false, "The instrument's number; the count of instruments adds a new one (the default).", {"instrument", "number", "id"}},
        {"name", "string", false, "Its name.", {}},
        {"kind", "string", false, "melodic or drum.", {"type"}},
        {"volume", "integers", false, "Loudness frame by frame, 0-15.", {"volumes", "envelope", "volume_envelope"}},
        {"volume_loop", "integer", false, "The frame the volume repeats from while the note is held (-1: it stays on the last).", {"loop"}},
        {"release", "integers", false, "Loudness frame by frame after the note is released.", {"releases"}},
        {"controls", "integers", false, "Melodic: the AUDC waveforms it may use (4 and 12 pure, 1 buzz, 6 and 7 bass).", {"audc", "waveforms", "control"}},
        {"pitch", "integers", false, "Melodic: cents added to the pitch frame by frame (vibrato, slides).", {"pitch_envelope", "vibrato"}},
        {"drum_control", "integers", false, "Drum: AUDC frame by frame (8 is noise).", {"drum_audc"}},
        {"drum_frequency", "integers", false, "Drum: AUDF frame by frame, 0-31.", {"drum_audf"}}}},
      {"music_set_pattern",
       {{"path", "string", true, "The .vcsmusic file (created if missing).", kPath},
        {"pattern", "integer", false, "The pattern's number (0 by default); the count of patterns adds a new one.", {"index", "number"}},
        {"channel", "integer", false, "0 or 1: the TIA's two voices (0 by default).", {"voice", "track"}},
        {"notes", "strings", true, "A token per row, separated by spaces: C4, F#3, Bb2 (C4 is middle C), . for nothing, off to release; C4:1 uses instrument 1.", {"cells", "melody", "sequence", "tokens"}},
        {"instrument", "integer", false, "The instrument of the notes that name none (0 by default).", {"default_instrument"}},
        {"rows", "integer", false, "The pattern's length in rows (as many as notes by default, 16 at least for a new one).", {"length"}},
        {"name", "string", false, "The pattern's name.", {}}}},
      {"music_set_effect",
       {{"path", "string", true, "The .vcsmusic file (created if missing).", kPath},
        {"name", "string", true, "What happens in the game: jump, shot, pickup...", {"effect", "sound"}},
        {"instrument", "integer", false, "The instrument that plays it (0 by default).", {}},
        {"note", "string", false, "The note it plays, such as C5 (C4 by default).", {"pitch"}},
        {"channel", "integer", false, "The channel it sounds on (1 by default).", {"voice"}}}},
      {"music_code",
       {{"path", "string", true, "The .vcsmusic file.", kPath}, {"format", "string", false, "dasm, bb, c, dpc or cdfj; the song's export by default.", {"language", "target"}}}},
      {"edit_file",
       {{"path", "string", true, "The file to change, relative to the project.", kPath},
        {"edits", "array", false, "Search-and-replace pairs [{\"search\": exact old text, \"replace\": new text}]; each search must occur once.", {"changes", "replacements", "patches"}},
        {"content", "string", false, "The whole text of a NEW file (instead of edits).", {"text", "new_content", "file_content", "body"}}}},
      {"build", {}},
      {"run_emulator", {}},
  };
  return list;
}

const Schema* schemaOf(const std::string& tool) {
  for (auto& item : schemas())
    if (tool == item.tool) return &item;
  return nullptr;
}

std::string squashed(const std::string& text) {
  std::string out;
  for (char c : text)
    if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
  return out;
}

int distance(const std::string& a, const std::string& b) {
  std::vector<int> previous(b.size() + 1), current(b.size() + 1);
  for (size_t j = 0; j <= b.size(); j++) previous[j] = (int)j;
  for (size_t i = 1; i <= a.size(); i++) {
    current[0] = (int)i;
    for (size_t j = 1; j <= b.size(); j++) current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
    std::swap(previous, current);
  }
  return previous[b.size()];
}

std::string normalisedKey(const std::string& key) {
  std::string out;
  for (char c : key) out += c == '-' || c == ' ' ? '_' : (char)std::tolower((unsigned char)c);
  return out;
}

// the value of a parameter in what the model sent: under its name or one of its aliases (the first found, not null)
const json* findValue(const json& args, const Param& param, std::string* usedKey) {
  std::vector<std::string> keys = {param.name};
  for (const char* alias : param.aliases) keys.push_back(alias);
  for (auto& wanted : keys)
    for (auto it = args.begin(); it != args.end(); ++it)
      if (normalisedKey(it.key()) == wanted && !it.value().is_null()) {
        *usedKey = it.key();
        return &it.value();
      }
  return nullptr;
}

bool coerceString(const json& value, std::string* out) {
  if (value.is_string()) *out = value.get<std::string>();
  else if (value.is_number_integer()) *out = std::to_string(value.get<long long>());
  else if (value.is_number()) *out = value.dump();
  else if (value.is_boolean()) *out = value.get<bool>() ? "true" : "false";
  else if (value.is_array() && value.size() == 1 && value[0].is_string()) *out = value[0].get<std::string>();
  else return false;
  return true;
}

bool coerceInteger(const json& value, int* out) {
  if (value.is_number_integer()) *out = (int)value.get<long long>();
  else if (value.is_number()) *out = (int)value.get<double>();
  else if (value.is_string()) {
    const std::string& text = value.get<std::string>();
    if (text.empty() || !std::all_of(text.begin() + (text[0] == '-' ? 1 : 0), text.end(), [](char c) { return std::isdigit((unsigned char)c); })) return false;
    *out = std::atoi(text.c_str());
  } else return false;
  return true;
}

// [{"search", "replace"}] from what a model wrote: the list, one pair, or a string holding either
bool coerceEdits(const json& value, json* out) {
  json list = value;
  if (list.is_string()) list = json::parse(list.get<std::string>(), nullptr, false);
  if (list.is_object()) list = json::array({list});
  if (!list.is_array() || list.empty()) return false;
  static const std::vector<const char*> searchNames = {"search", "old", "old_text", "old_string", "find", "before", "original", "from"};
  static const std::vector<const char*> replaceNames = {"replace", "new", "new_text", "new_string", "with", "after", "replacement", "to"};
  *out = json::array();
  for (auto& item : list) {
    if (!item.is_object()) return false;
    json pair = json::object();
    for (auto it = item.begin(); it != item.end(); ++it) {
      std::string key = normalisedKey(it.key());
      if (!it.value().is_string()) continue;
      if (!pair.contains("search") && std::find_if(searchNames.begin(), searchNames.end(), [&](const char* n) { return key == n; }) != searchNames.end()) pair["search"] = it.value();
      else if (!pair.contains("replace") && std::find_if(replaceNames.begin(), replaceNames.end(), [&](const char* n) { return key == n; }) != replaceNames.end()) pair["replace"] = it.value();
    }
    if (!pair.contains("search") || !pair.contains("replace")) return false;
    out->push_back(pair);
  }
  return true;
}

std::string shortDump(const json& value) {
  std::string text = value.dump(-1, ' ', false, json::error_handler_t::replace);
  return text.size() > 240 ? text.substr(0, 240) + "..." : text;
}
}  // namespace

std::string resolveToolName(const std::string& written) {
  std::string text = written;
  for (const char* prefix : {"functions.", "functions/", "default_api.", "tools.", "tool."})
    if (text.rfind(prefix, 0) == 0) text = text.substr(std::string(prefix).size());
  std::string key = squashed(text);
  if (key.empty()) return "";
  for (auto& item : schemas())
    if (squashed(item.tool) == key) return item.tool;
  // one or two letters off, or a word added: the closest, if there is only one
  std::string best;
  int bestDistance = 3;
  bool tie = false;
  for (auto& item : schemas()) {
    int d = distance(key, squashed(item.tool));
    if (key.size() >= 5 && d < bestDistance) bestDistance = d, best = item.tool, tie = false;
    else if (key.size() >= 5 && d == bestDistance) tie = true;
  }
  return tie ? "" : best;
}

CallCheck repairCall(ToolCall* call) {
  CallCheck check;
  std::string real = resolveToolName(call->name);
  if (real.empty()) {
    check.ok = false;
    check.message = "there is no tool called \"" + call->name + "\"";
    return check;
  }
  if (real != call->name) check.notes.push_back("the tool is called " + real + ", not " + call->name);
  call->name = real;
  const Schema* schema = schemaOf(real);
  json received = call->args;
  json args = call->args;
  if (args.is_string()) args = json::parse(args.get<std::string>(), nullptr, false);  // the arguments as a JSON string
  if (args.is_null() || args.is_discarded() || !args.is_object()) args = json::object();
  json fixed = json::object();
  std::vector<std::string> used;
  std::vector<std::string> problems;
  for (auto& param : schema->params) {
    std::string key;
    const json* value = findValue(args, param, &key);
    if (!value) continue;
    std::string type = param.type;
    bool good = false;
    if (type == "string") {
      std::string text;
      if ((good = coerceString(*value, &text))) fixed[param.name] = text;
    } else if (type == "integer") {
      int number = 0;
      if ((good = coerceInteger(*value, &number))) fixed[param.name] = number;
    } else if (type == "boolean") {
      if (value->is_boolean()) fixed[param.name] = value->get<bool>(), good = true;
      else if (value->is_number()) fixed[param.name] = value->get<double>() != 0, good = true;
      else if (value->is_string()) {
        std::string text = normalisedKey(value->get<std::string>());
        if (text == "true" || text == "yes" || text == "1" || text == "on") fixed[param.name] = true, good = true;
        else if (text == "false" || text == "no" || text == "0" || text == "off") fixed[param.name] = false, good = true;
      }
    } else if (type == "strings") {  // a list of texts, or one text (the tool splits it by lines)
      if (value->is_string()) fixed[param.name] = *value, good = true;
      else if (value->is_array()) {
        json list = json::array();
        good = true;
        for (auto& item : *value) {
          std::string text;
          if (coerceString(item, &text)) list.push_back(text);
          else good = false;
        }
        if (good) fixed[param.name] = list;
      }
    } else if (type == "integers") {  // a list of numbers, one number, or a text of numbers
      json list = json::array();
      if (value->is_array()) {
        good = true;
        for (auto& item : *value) {
          int number = 0;
          if (coerceInteger(item, &number)) list.push_back(number);
          else good = false;
        }
      } else if (value->is_number()) {
        list.push_back((int)value->get<double>()), good = true;
      } else if (value->is_string()) {
        std::string text = value->get<std::string>();
        for (auto& c : text)
          if (c == ',' || c == ';' || c == '[' || c == ']') c = ' ';
        std::istringstream in(text);
        good = true;
        for (std::string token; in >> token;) {
          int number = 0;
          if (coerceInteger(json(token), &number)) list.push_back(number);
          else good = false;
        }
      }
      if (good) fixed[param.name] = list;
    } else {
      json edits;
      if ((good = coerceEdits(*value, &edits))) fixed[param.name] = edits;
    }
    if (!good)
      problems.push_back(std::string("\"") + param.name + "\" must be " +
                         (type == "integer" ? "a number" : type == "array" ? "a list of {\"search\", \"replace\"} pairs" : type == "boolean" ? "true or false" : type == "strings" ? "a list of texts"
                          : type == "integers" ? "a list of numbers" : "text"));
    else if (key != param.name) check.notes.push_back("\"" + key + "\" taken as \"" + param.name + "\"");
    else if (!value->is_string() && type == "string") check.notes.push_back(std::string("\"") + param.name + "\" was not text");
    used.push_back(key);
  }
  // one required text missing and exactly one unknown text in the call: that is it
  for (auto& param : schema->params) {
    if (!param.required || fixed.contains(param.name) || std::string(param.type) != "string") continue;
    std::string only, onlyKey;
    int candidates = 0;
    for (auto it = args.begin(); it != args.end(); ++it)
      if (it.value().is_string() && !it.value().get<std::string>().empty() && std::find(used.begin(), used.end(), it.key()) == used.end()) only = it.value().get<std::string>(), onlyKey = it.key(), candidates++;
    if (candidates == 1) {
      fixed[param.name] = only;
      used.push_back(onlyKey);
      check.notes.push_back("\"" + onlyKey + "\" taken as \"" + param.name + "\"");
    }
  }
  // required fields, and the rule of edit_file (edits or content)
  for (auto& param : schema->params)
    if (param.required && !fixed.contains(param.name)) problems.push_back(std::string("missing \"") + param.name + "\" (" + param.type + ": " + param.description + ")");
  if (real == "edit_file" && !fixed.contains("edits") && !fixed.contains("content")) problems.push_back("give \"edits\" (to change a file that exists) or \"content\" (to create a new one)");
  if (real == "edit_file" && fixed.contains("edits") && fixed.contains("content")) fixed.erase("content");
  if (!problems.empty()) {
    check.ok = false;
    check.message = "invalid arguments for " + real + ": ";
    for (size_t i = 0; i < problems.size(); i++) check.message += (i ? "; " : "") + problems[i];
    check.message += ". Received: " + shortDump(received.is_null() ? json::object() : received) + ". Fix the arguments and call the tool again.";
    return check;
  }
  call->args = fixed;
  return check;
}

json nativeTools(const std::vector<std::string>& names, bool anthropic) {
  json list = json::array();
  for (auto& name : names) {
    const Schema* schema = schemaOf(name);
    if (!schema) continue;
    json properties = json::object(), required = json::array();
    for (auto& param : schema->params) {
      std::string kind = param.type;
      json property = {{"type", kind == "strings" || kind == "integers" ? std::string("array") : kind}, {"description", param.description}};
      if (kind == "strings") property["items"] = {{"type", "string"}};
      if (kind == "integers") property["items"] = {{"type", "integer"}};
      if (kind == "array") property["items"] = {{"type", "object"},
                                                                    {"properties", {{"search", {{"type", "string"}}}, {"replace", {{"type", "string"}}}}},
                                                                    {"required", json::array({"search", "replace"})}};
      properties[param.name] = property;
      if (param.required) required.push_back(param.name);
    }
    json parameters = {{"type", "object"}, {"properties", properties}, {"required", required}};
    std::string description = toolDescription(name);
    if (anthropic) list.push_back({{"name", name}, {"description", description}, {"input_schema", parameters}});
    else list.push_back({{"type", "function"}, {"function", {{"name", name}, {"description", description}, {"parameters", parameters}}}});
  }
  return list;
}

}  // namespace vcs::assistant
