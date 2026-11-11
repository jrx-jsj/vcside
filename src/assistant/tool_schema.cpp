// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/tool_schema.h"

#include <algorithm>
#include <cctype>

namespace vcs::assistant {

using json = nlohmann::json;

namespace {

struct Param {
  const char* name;
  const char* type;  // "string", "integer", "array"
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
    } else {
      json edits;
      if ((good = coerceEdits(*value, &edits))) fixed[param.name] = edits;
    }
    if (!good) problems.push_back(std::string("\"") + param.name + "\" must be " + (type == "integer" ? "a number" : type == "array" ? "a list of {\"search\", \"replace\"} pairs" : "text"));
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
      json property = {{"type", param.type}, {"description", param.description}};
      if (std::string(param.type) == "array") property["items"] = {{"type", "object"},
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
