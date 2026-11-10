// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/prompt.h"

namespace vcs::assistant {

int estimateTokens(const std::string& text) { return (int)(text.size() * 2 / 7) + 1; }

namespace {
// A file or selection that is too long keeps its start and its end.
std::string limited(const std::string& text, size_t limit) {
  if (text.size() <= limit) return text;
  size_t half = limit / 2;
  return text.substr(0, half) + "\n... [" + std::to_string(text.size() - limit) + " characters left out] ...\n" + text.substr(text.size() - half);
}
}  // namespace

std::string userTurn(const std::string& question, const Attachments& attachments) {
  std::string turn;
  if (!attachments.projectInfo.empty()) turn += "Project: " + attachments.projectInfo + "\n\n";
  if (!attachments.grounding.empty()) turn += attachments.grounding + "\n";
  if (!attachments.fileText.empty()) turn += "File " + attachments.fileName + " (data):\n```\n" + limited(attachments.fileText, 12000) + "\n```\n\n";
  if (!attachments.selection.empty()) turn += "Selected code (data):\n```\n" + limited(attachments.selection, 6000) + "\n```\n\n";
  if (!attachments.errors.empty()) turn += "Build errors (data):\n```\n" + limited(attachments.errors, 3000) + "\n```\n\n";
  return turn + question;
}

std::vector<Message> buildMessages(const std::string& systemPrompt, const std::vector<ChatMessage>& history, const std::string& turn,
                                   int contextTokens, int replyReserve, int* droppedTurns) {
  int budget = contextTokens - replyReserve - estimateTokens(systemPrompt) - estimateTokens(turn);
  // keep the newest turns that fit
  size_t first = history.size();
  int used = 0;
  while (first > 0) {
    int cost = estimateTokens(history[first - 1].content);
    if (used + cost > budget) break;
    used += cost;
    first--;
  }
  // do not start on an answer whose question was dropped
  while (first < history.size() && history[first].role != "user") first++;
  if (droppedTurns) *droppedTurns = (int)first;
  std::vector<Message> messages;
  messages.push_back({"system", systemPrompt});
  for (size_t i = first; i < history.size(); i++) messages.push_back({history[i].role, history[i].content});
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
