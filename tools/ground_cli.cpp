// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Prints what the assistant would add to a question from the knowledge base, as JSON:
//   vcside_ground <data dir> "<question>"  ->  {"entries":["WSYNC",...],"block":"..."}
// Used by tools/assistant_eval/run.py to measure how much the knowledge base helps a model.
#include <cstdio>
#include <nlohmann/json.hpp>

#include "assistant/grounding.h"
#include "kb.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: vcside_ground <data dir> <question>\n");
    return 2;
  }
  vcs::KnowledgeBase kb;
  std::string error;
  kb.load(std::string(argv[1]) + "/kb", &error);
  auto entries = vcs::assistant::groundText(kb, argv[2], "", vcs::assistant::GroundingContext());
  nlohmann::json out;
  out["entries"] = nlohmann::json::array();
  for (auto& entry : entries) out["entries"].push_back(entry.id);
  out["block"] = vcs::assistant::groundingBlock(entries);
  std::printf("%s\n", out.dump().c_str());
  return 0;
}
