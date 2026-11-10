// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/edits.h"

#include <algorithm>
#include <sstream>

namespace vcs::assistant {

namespace {
std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return text;
}

std::string trimmed(const std::string& text) {
  size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string::npos) return "";
  return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// "<<<<<<< SEARCH", ">>>>>>> REPLACE" and "=======": five or more of the sign, then the word
enum class Marker { None, Search, Divider, Replace };
Marker markerOf(const std::string& line) {
  std::string text = lowered(trimmed(line));
  auto run = [&](char sign) { size_t n = 0; while (n < text.size() && text[n] == sign) n++; return n; };
  if (run('<') >= 5 && text.find("search", run('<')) != std::string::npos) return Marker::Search;
  if (run('>') >= 5 && text.find("replace", run('>')) != std::string::npos) return Marker::Replace;
  if (run('=') >= 5 && run('=') == text.size()) return Marker::Divider;
  return Marker::None;
}

std::string joined(const std::vector<std::string>& lines) {
  std::string text;
  for (size_t i = 0; i < lines.size(); i++) text += (i ? "\n" : "") + lines[i];
  return text;
}
}  // namespace

EditParse parseEditBlock(const std::string& info, const std::string& body, EditBlock* block, std::string* problem) {
  std::istringstream infoWords(info);
  std::string first, rest, path;
  infoWords >> first;
  std::getline(infoWords, rest);
  bool named = lowered(first) == "edit";
  path = trimmed(rest);
  // does the body hold search-and-replace sections at all?
  std::vector<std::string> lines;
  {
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  bool hasMarkers = false;
  for (auto& line : lines) hasMarkers = hasMarkers || markerOf(line) == Marker::Search;
  if (!named && !hasMarkers) return EditParse::NotAnEdit;
  if (!named) {  // "asm src/main.asm" or just "src/main.asm" with the markers inside
    std::istringstream all(info);
    std::string word;
    while (all >> word)
      if (word.find('.') != std::string::npos || word.find('/') != std::string::npos) path = word;
  }
  auto broken = [&](const std::string& why) {
    if (problem) *problem = why;
    return EditParse::Broken;
  };
  if (path.empty()) return broken("the block does not say which file it changes");
  if (!hasMarkers) return broken("the block has no SEARCH and REPLACE sections");
  EditBlock result;
  result.path = path;
  enum class State { Outside, InSearch, InReplace } state = State::Outside;
  std::vector<std::string> search, replace;
  for (auto& line : lines) {
    Marker marker = markerOf(line);
    switch (state) {
      case State::Outside:
        if (marker == Marker::Search) state = State::InSearch, search.clear(), replace.clear();
        break;
      case State::InSearch:
        if (marker == Marker::Divider) state = State::InReplace;
        else if (marker == Marker::Search || marker == Marker::Replace) return broken("a SEARCH section has no divider (=======)");
        else search.push_back(line);
        break;
      case State::InReplace:
        if (marker == Marker::Replace) {
          result.sections.push_back({joined(search), joined(replace)});
          state = State::Outside;
        } else if (marker == Marker::Search || marker == Marker::Divider) {
          return broken("a REPLACE section is not closed");
        } else {
          replace.push_back(line);
        }
        break;
    }
  }
  if (state != State::Outside) return broken("the block is cut off before its last REPLACE marker");
  if (result.sections.empty()) return broken("the block has no SEARCH and REPLACE sections");
  *block = result;
  return EditParse::Edit;
}

nlohmann::json editArguments(const EditBlock& block) {
  nlohmann::json args = {{"path", block.path}};
  if (block.sections.size() == 1 && block.sections[0].search.empty()) {  // a new file
    args["content"] = block.sections[0].replace + "\n";
    return args;
  }
  args["edits"] = nlohmann::json::array();
  for (auto& section : block.sections) args["edits"].push_back({{"search", section.search}, {"replace", section.replace}});
  return args;
}

}  // namespace vcs::assistant
