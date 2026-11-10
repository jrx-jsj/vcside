// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The part of Markdown that models actually write in a chat answer: headings, paragraphs,
// bullet and numbered lists, quotes, tables, rules, and **bold**, *italic*, `code` and [links](url)
// inside them. Code fences are split off earlier (splitBlocks) and are not handled here.
// Parsing is separate from drawing so it can be tested; the panel draws the result.
#pragma once
#include <string>
#include <vector>

namespace vcs::assistant {

struct MdRun {
  std::string text;
  bool bold = false, italic = false, code = false;
};

struct MdBlock {
  enum Kind { Paragraph, Heading, Bullet, Number, Quote, Rule, Table } kind = Paragraph;
  int level = 0;                       // Heading: 1-6; Bullet and Number: nesting depth, from 0
  int number = 0;                      // Number: the number the author wrote
  std::vector<MdRun> runs;             // everything but Rule and Table
  std::vector<std::vector<std::vector<MdRun>>> rows;  // Table: rows of cells (the header is row 0)
};

std::vector<MdBlock> parseMarkdown(const std::string& text);
// Only the inline marks: **bold**, *italic*, `code`, [text](url) (the text is kept).
std::vector<MdRun> parseInline(const std::string& text);

}  // namespace vcs::assistant
