// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Assembling what is sent to the model: the system prompt, the history trimmed to the
// model's window, and the attached context (file, selection, errors, knowledge-base entries).
#pragma once
#include <string>
#include <vector>

#include "assistant/provider.h"
#include "assistant/settings.h"

namespace vcs::assistant {

// Text that goes into JSON must be valid UTF-8 (nlohmann::json throws when it dumps anything else,
// and an uncaught throw ends the program). sanitizeUtf8 replaces every invalid byte with U+FFFD;
// utf8Cut is the longest start of the text of at most maxBytes that does not split a character;
// utf8Tail the end, from a character boundary.
std::string sanitizeUtf8(const std::string& text);
std::string utf8Cut(const std::string& text, size_t maxBytes);
std::string utf8Tail(const std::string& text, size_t maxBytes);

// Rough token count (characters / 3.5): enough to trim; a server's own usage report wins when shown.
int estimateTokens(const std::string& text);

struct Attachments {
  std::string projectInfo;   // a few lines: language, format, TV, main file
  std::string fileName, fileText;  // the open file (empty = not attached)
  std::string selection, selectionLabel;  // the selected code and where it is ("main.asm:12-20")
  std::vector<std::pair<std::string, std::string>> extraFiles;  // files added by hand: path, text
  std::string errors;
  std::string grounding;     // groundingBlock()
  std::string projectMap;    // the files and the names they define
  std::string excerpts;      // chunks of the project that match the question (already formatted)
};

// The text of the user's turn with the attachments in front of it, each labelled as data.
std::string userTurn(const std::string& question, const Attachments& attachments);

// System prompt + history (older turns dropped first, whole turns only) + this turn.
// history holds the earlier turns as they were sent (attachments included); the newest
// question is `turn`. replyReserve tokens are kept for the answer.
std::vector<Message> buildMessages(const std::string& systemPrompt, const std::vector<ChatMessage>& history,
                                   const std::string& turn, int contextTokens, int replyReserve = 1024,
                                   int* droppedTurns = nullptr);

// Code blocks of a Markdown-ish answer, in order; text between them is "prose".
struct Block {
  bool code = false;
  std::string language, text;
};
std::vector<Block> splitBlocks(const std::string& answer);

}  // namespace vcs::assistant
