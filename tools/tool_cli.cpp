// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Runs the agent's tools outside the IDE, for tools/assistant_eval/agent_run.py:
//   vcside_tool <data dir> <project dir> prompt <profile>   -> the tool section of the system prompt
//   vcside_tool <data dir> <project dir> call               -> reads one call (the model's answer) from stdin,
//        prints {"found":bool,"tool":..,"text":..,"error":bool,"proposed":bool,"path":..,"after":..}
#include <cstdio>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

#include "assistant/project_index.h"
#include "assistant/prompt.h"
#include "assistant/tools.h"
#include "kb.h"

int main(int argc, char** argv) {
  using namespace vcs::assistant;
  if (argc < 4) {
    std::fprintf(stderr, "usage: vcside_tool <data dir> <project dir> prompt <profile> | call\n");
    return 2;
  }
  std::string mode = argv[3];
  if (mode == "prompt") {
    std::printf("%s", toolPrompt(argc > 4 ? std::atoi(argv[4]) : 1).c_str());
    return 0;
  }
  vcs::KnowledgeBase kb;
  std::string error;
  kb.load(std::string(argv[1]) + "/kb", &error);
  ToolEnv env;
  env.projectDir = argv[2];
  env.kb = &kb;
  env.projectInfo = "DASM, 4K, NTSC, main.asm";
  ProjectIndex index;
  env.findCode = [&](const std::string& query) {
    index.update(env.projectDir);
    std::string out;
    for (auto& hit : index.search(query, 4)) out += hit.file + ":" + std::to_string(hit.firstLine) + "-" + std::to_string(hit.lastLine) + "\n" + hit.text + "\n";
    return out;
  };
  std::string answer((std::istreambuf_iterator<char>(std::cin)), {});
  if (mode == "context") {  // what the panel puts in front of a question: the excerpts and the map
    Attachments attachments;
    attachments.projectInfo = env.projectInfo;
    index.update(env.projectDir);
    for (auto& hit : index.search(answer, 3)) attachments.excerpts += "--- " + hit.file + ":" + std::to_string(hit.firstLine) + "-" + std::to_string(hit.lastLine) + "\n```\n" + hit.text + "\n```\n";
    attachments.projectMap = index.map(answer, 900);
    std::printf("%s", userTurn(answer, attachments).c_str());
    return 0;
  }
  ToolCall call;
  nlohmann::json out = {{"found", false}};
  if (findToolCall(answer, &call)) {
    ToolResult result = runTool(env, call);
    out = {{"found", true}, {"tool", describeCall(call)}, {"text", result.text}, {"error", result.error}, {"proposed", result.proposed}};
    if (result.proposed) out["path"] = result.edit.path, out["after"] = result.edit.after;
  }
  std::printf("%s\n", out.dump().c_str());
  return 0;
}
