// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Draws parsed Markdown with ImGui (headings, lists, quotes, tables, **bold**, `code`): the assistant's answers and the
// documentation cards. The words are laid out by hand so bold, code and plain text share a line and wrap together.
#pragma once
#include <functional>
#include <string>

struct ImFont;

namespace vcs {
class KnowledgeBase;
}

namespace vcs::assistant {

// Registers and instructions named in the text open their entry in the documentation. Set while a view is drawn (nullptr: no
// links); never inside code blocks.
struct MdLinks {
  const KnowledgeBase* kb = nullptr;
  std::function<void(const std::string&)> open;
};
void setMarkdownLinks(const MdLinks* links);

// The Markdown without code fences.
void drawMarkdown(const std::string& text, ImFont* mono);
// With code fences: prose drawn as Markdown, each fence in a framed block.
void drawMarkdownWithCode(const std::string& text, ImFont* mono);

}  // namespace vcs::assistant
