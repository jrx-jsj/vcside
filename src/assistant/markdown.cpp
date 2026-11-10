// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/markdown.h"

#include <cctype>

namespace vcs::assistant {

namespace {

std::string trim(const std::string& text) {
  size_t start = text.find_first_not_of(" \t\r");
  if (start == std::string::npos) return "";
  size_t end = text.find_last_not_of(" \t\r");
  return text.substr(start, end - start + 1);
}

size_t leadingSpaces(const std::string& line) {
  size_t count = 0;
  for (char c : line) {
    if (c == ' ') count++;
    else if (c == '\t') count += 4;
    else break;
  }
  return count;
}

bool isRule(const std::string& trimmed) {
  if (trimmed.size() < 3) return false;
  char mark = trimmed[0];
  if (mark != '-' && mark != '*' && mark != '_') return false;
  for (char c : trimmed)
    if (c != mark && c != ' ') return false;
  return true;
}

// "- item", "* item", "+ item"
bool bulletStart(const std::string& trimmed, std::string* rest) {
  if (trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') && trimmed[1] == ' ') {
    *rest = trim(trimmed.substr(2));
    return true;
  }
  return false;
}

// "1. item", "2) item"
bool numberStart(const std::string& trimmed, int* number, std::string* rest) {
  size_t digits = 0;
  while (digits < trimmed.size() && digits < 4 && std::isdigit((unsigned char)trimmed[digits])) digits++;
  if (digits == 0 || digits + 1 >= trimmed.size()) return false;
  if ((trimmed[digits] != '.' && trimmed[digits] != ')') || trimmed[digits + 1] != ' ') return false;
  *number = std::stoi(trimmed.substr(0, digits));
  *rest = trim(trimmed.substr(digits + 2));
  return true;
}

bool headingStart(const std::string& trimmed, int* level, std::string* rest) {
  size_t hashes = 0;
  while (hashes < trimmed.size() && trimmed[hashes] == '#') hashes++;
  if (hashes == 0 || hashes > 6 || hashes >= trimmed.size() || trimmed[hashes] != ' ') return false;
  *level = (int)hashes;
  *rest = trim(trimmed.substr(hashes + 1));
  while (!rest->empty() && rest->back() == '#') rest->pop_back();  // "## Title ##"
  *rest = trim(*rest);
  return true;
}

bool tableRow(const std::string& trimmed) { return trimmed.size() >= 2 && trimmed[0] == '|'; }

bool tableSeparator(const std::string& trimmed) {
  if (!tableRow(trimmed)) return false;
  bool dash = false;
  for (char c : trimmed) {
    if (c == '-') dash = true;
    else if (c != '|' && c != ':' && c != ' ') return false;
  }
  return dash;
}

std::vector<std::string> splitCells(const std::string& trimmed) {
  std::vector<std::string> cells;
  std::string current;
  size_t start = trimmed[0] == '|' ? 1 : 0;
  for (size_t i = start; i < trimmed.size(); i++) {
    if (trimmed[i] == '|') {
      cells.push_back(trim(current));
      current.clear();
    } else {
      current += trimmed[i];
    }
  }
  if (!trim(current).empty()) cells.push_back(trim(current));
  return cells;
}

void addRun(std::vector<MdRun>& runs, const std::string& text, bool bold, bool italic, bool code) {
  if (text.empty()) return;
  if (!runs.empty() && runs.back().bold == bold && runs.back().italic == italic && runs.back().code == code) runs.back().text += text;
  else runs.push_back({text, bold, italic, code});
}

}  // namespace

std::vector<MdRun> parseInline(const std::string& text) {
  std::vector<MdRun> runs;
  bool bold = false, italic = false;
  std::string plain;
  auto flush = [&] {
    addRun(runs, plain, bold, italic, false);
    plain.clear();
  };
  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    if (c == '\\' && i + 1 < text.size() && std::string("\\`*_{}[]()#+-.!|").find(text[i + 1]) != std::string::npos) {
      plain += text[++i];
      continue;
    }
    if (c == '`') {
      size_t close = text.find('`', i + 1);
      if (close != std::string::npos && close > i + 1) {
        flush();
        addRun(runs, text.substr(i + 1, close - i - 1), bold, italic, true);
        i = close;
        continue;
      }
    }
    if ((c == '*' || c == '_') && i + 1 < text.size() && text[i + 1] == c) {  // ** or __
      // an opening mark needs a closing one later; "__init__" style names are left alone
      bool before = i == 0 || !std::isalnum((unsigned char)text[i - 1]);
      if (bold || ((c == '*' || before) && text.find(std::string(2, c), i + 2) != std::string::npos)) {
        flush();
        bold = !bold;
        i++;
        continue;
      }
    }
    if (c == '*' && i + 1 < text.size() && text[i + 1] != ' ' && text[i + 1] != '*') {  // *italic*, not "2 * 3"
      if (italic || text.find('*', i + 1) != std::string::npos) {
        flush();
        italic = !italic;
        continue;
      }
    }
    if (c == '[') {  // [text](url): keep the text
      size_t close = text.find(']', i + 1);
      if (close != std::string::npos && close + 1 < text.size() && text[close + 1] == '(') {
        size_t end = text.find(')', close + 2);
        if (end != std::string::npos) {
          plain += text.substr(i + 1, close - i - 1);
          i = end;
          continue;
        }
      }
    }
    plain += c;
  }
  flush();
  return runs;
}

std::vector<MdBlock> parseMarkdown(const std::string& text) {
  std::vector<MdBlock> blocks;
  std::vector<std::string> lines;
  {
    std::string current;
    for (char c : text) {
      if (c == '\n') lines.push_back(current), current.clear();
      else current += c;
    }
    lines.push_back(current);
  }
  // text of the block being continued (a paragraph, list item or quote), joined with spaces
  std::string pending;
  MdBlock pendingBlock;
  bool open = false;
  auto close = [&] {
    if (!open) return;
    pendingBlock.runs = parseInline(pending);
    if (!pendingBlock.runs.empty()) blocks.push_back(pendingBlock);
    open = false, pending.clear();
  };
  auto begin = [&](MdBlock::Kind kind, int level, const std::string& first) {
    close();
    pendingBlock = MdBlock();
    pendingBlock.kind = kind, pendingBlock.level = level;
    pending = first, open = true;
  };

  for (size_t i = 0; i < lines.size(); i++) {
    const std::string& line = lines[i];
    std::string trimmed = trim(line);
    int number = 0, level = 0;
    std::string rest;
    if (trimmed.empty()) {
      close();
    } else if (isRule(trimmed)) {
      close();
      MdBlock rule;
      rule.kind = MdBlock::Rule;
      blocks.push_back(rule);
    } else if (headingStart(trimmed, &level, &rest)) {
      begin(MdBlock::Heading, level, rest);
      close();
    } else if (tableRow(trimmed)) {
      close();
      MdBlock table;
      table.kind = MdBlock::Table;
      while (i < lines.size() && tableRow(trim(lines[i]))) {
        std::string row = trim(lines[i]);
        if (!tableSeparator(row)) {
          std::vector<std::vector<MdRun>> cells;
          for (auto& cell : splitCells(row)) cells.push_back(parseInline(cell));
          table.rows.push_back(cells);
        }
        i++;
      }
      i--;
      if (!table.rows.empty()) blocks.push_back(table);
    } else if (bulletStart(trimmed, &rest)) {
      begin(MdBlock::Bullet, (int)(leadingSpaces(line) / 2), rest);
    } else if (numberStart(trimmed, &number, &rest)) {
      begin(MdBlock::Number, (int)(leadingSpaces(line) / 2), rest);
      pendingBlock.number = number;
    } else if (trimmed[0] == '>') {
      std::string quote = trim(trimmed.substr(1));
      if (open && pendingBlock.kind == MdBlock::Quote) pending += " " + quote;
      else begin(MdBlock::Quote, 0, quote);
    } else if (open) {
      pending += " " + trimmed;  // a wrapped line continues the block above
    } else {
      begin(MdBlock::Paragraph, 0, trimmed);
    }
  }
  close();
  return blocks;
}

}  // namespace vcs::assistant
