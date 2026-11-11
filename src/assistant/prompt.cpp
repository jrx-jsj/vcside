// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/prompt.h"

namespace vcs::assistant {

int estimateTokens(const std::string& text) { return (int)(text.size() * 2 / 7) + 1; }

std::string sanitizeUtf8(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    unsigned char lead = (unsigned char)text[i];
    size_t length = lead < 0x80 ? 1 : (lead >= 0xC2 && lead <= 0xDF) ? 2 : (lead >= 0xE0 && lead <= 0xEF) ? 3 : (lead >= 0xF0 && lead <= 0xF4) ? 4 : 0;
    bool valid = length > 0 && i + length <= text.size();
    for (size_t k = 1; valid && k < length; k++) valid = ((unsigned char)text[i + k] & 0xC0) == 0x80;
    if (valid && length == 3) {  // no overlong forms, no surrogates
      unsigned char second = (unsigned char)text[i + 1];
      if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second >= 0xA0)) valid = false;
    }
    if (valid && length == 4) {  // 0x10000 to 0x10FFFF only
      unsigned char second = (unsigned char)text[i + 1];
      if ((lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second >= 0x90)) valid = false;
    }
    if (valid) {
      out.append(text, i, length);
      i += length;
    } else {
      out += "\xEF\xBF\xBD";
      i++;
    }
  }
  return out;
}

std::string utf8Cut(const std::string& text, size_t maxBytes) {
  if (text.size() <= maxBytes) return text;
  size_t end = maxBytes;
  while (end > 0 && ((unsigned char)text[end] & 0xC0) == 0x80) end--;  // text[end] would start the part left out
  return text.substr(0, end);
}

std::string utf8Tail(const std::string& text, size_t maxBytes) {
  if (text.size() <= maxBytes) return text;
  size_t start = text.size() - maxBytes;
  while (start < text.size() && ((unsigned char)text[start] & 0xC0) == 0x80) start++;
  return text.substr(start);
}

namespace {
// A file or selection that is too long keeps its start and its end.
std::string limited(const std::string& text, size_t limit) {
  if (text.size() <= limit) return text;
  size_t half = limit / 2;
  return utf8Cut(text, half) + "\n... [" + std::to_string(text.size() - limit) + " characters left out] ...\n" + utf8Tail(text, half);
}
}  // namespace

std::string userTurn(const std::string& question, const Attachments& attachments) {
  std::string turn;
  if (!attachments.projectInfo.empty()) turn += "Project: " + attachments.projectInfo + "\n\n";
  if (!attachments.projectNotes.empty()) turn += "Notes from the author of this project (the file VCSIDE.md: rules and facts of this project that the code does not show; follow them):\n" + limited(attachments.projectNotes, 4000) + "\n\n";
  if (!attachments.projectMap.empty()) turn += "Project files and the names they define (data):\n" + attachments.projectMap + "\n";
  if (!attachments.grounding.empty()) turn += attachments.grounding + "\n";
  if (!attachments.excerpts.empty()) turn += "Parts of the project that match your question (data, found by searching its files):\n" + attachments.excerpts + "\n";
  if (!attachments.fileText.empty()) turn += "File " + attachments.fileName + " (data):\n```\n" + limited(attachments.fileText, 12000) + "\n```\n\n";
  for (auto& file : attachments.extraFiles) turn += "File " + file.first + " (data):\n```\n" + limited(file.second, 12000) + "\n```\n\n";
  if (!attachments.fileText.empty() || !attachments.extraFiles.empty()) {  // the name to write in a change: the model invents "src/main.asm" otherwise
    turn += "To change a file, use its path exactly as written above after \"File\"";
    turn += attachments.fileText.empty() ? std::string(".") : ": " + attachments.fileName + ".";
    turn += "\n\n";
  }
  if (!attachments.selection.empty())
    turn += "Selected code" + (attachments.selectionLabel.empty() ? std::string() : " in " + attachments.selectionLabel) + " (data):\n```\n" + limited(attachments.selection, 12000) + "\n```\n\n";
  if (!attachments.errors.empty()) turn += "Build errors (data):\n```\n" + limited(attachments.errors, 3000) + "\n```\n\n";
  return sanitizeUtf8(turn + question);
}

std::vector<Message> buildMessages(const std::string& systemPrompt, const std::vector<ChatMessage>& history, const std::string& turn,
                                   int contextTokens, int replyReserve, int* droppedTurns) {
  int budget = contextTokens - replyReserve - estimateTokens(systemPrompt) - estimateTokens(turn);
  // keep the newest turns that fit
  size_t first = history.size();
  int used = 0;
  while (first > 0) {
    int cost = estimateTokens(history[first - 1].english.empty() ? history[first - 1].content : history[first - 1].english);
    if (used + cost > budget) break;
    used += cost;
    first--;
  }
  // do not start on an answer whose question was dropped
  while (first < history.size() && history[first].role != "user") first++;
  if (droppedTurns) *droppedTurns = (int)first;
  std::vector<Message> messages;
  messages.push_back({"system", systemPrompt});
  for (size_t i = first; i < history.size(); i++) messages.push_back({history[i].role, history[i].english.empty() ? history[i].content : history[i].english});
  messages.push_back({"user", turn});
  return messages;
}

std::vector<Block> splitBlocks(const std::string& answer) {
  std::vector<Block> blocks;
  size_t pos = 0;
  while (pos < answer.size()) {
    size_t fence = answer.find("```", pos);
    if (fence == std::string::npos) {
      blocks.push_back({false, "", answer.substr(pos)});
      break;
    }
    if (fence > pos) blocks.push_back({false, "", answer.substr(pos, fence - pos)});
    size_t lineEnd = answer.find('\n', fence);
    if (lineEnd == std::string::npos) {  // an unfinished fence at the end of a streaming answer
      blocks.push_back({true, answer.substr(fence + 3), ""});
      break;
    }
    Block block;
    block.code = true;
    block.language = answer.substr(fence + 3, lineEnd - fence - 3);
    size_t close = answer.find("```", lineEnd + 1);
    if (close == std::string::npos) {
      block.text = answer.substr(lineEnd + 1);
      blocks.push_back(block);
      break;
    }
    block.text = answer.substr(lineEnd + 1, close - lineEnd - 1);
    if (!block.text.empty() && block.text.back() == '\n') block.text.pop_back();
    blocks.push_back(block);
    pos = close + 3;
    if (pos < answer.size() && answer[pos] == '\n') pos++;
  }
  return blocks;
}

}  // namespace vcs::assistant
