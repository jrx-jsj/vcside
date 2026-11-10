// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Runs the project index outside the IDE, for tools/assistant_eval/index_run.py:
//   vcside_index <project dir> <queries file, one per line> [--url <embedding server /v1>] [--model <id>]
//                [--query-prefix <text>] [--doc-prefix <text>] [--min <similarity>] [--top <n>]
// Prints one JSON object per query: the best chunks by keywords alone and, with --url, by keywords
// and meaning together.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "assistant/embeddings.h"
#include "assistant/project_index.h"

using namespace vcs::assistant;

static nlohmann::json hitsJson(const std::vector<IndexHit>& hits) {
  nlohmann::json list = nlohmann::json::array();
  for (auto& hit : hits) list.push_back({{"file", hit.file}, {"first", hit.firstLine}, {"last", hit.lastLine}, {"score", hit.score}});
  return list;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: vcside_index <project dir> <queries file> [--url U] [--model ID] [--query-prefix P] [--doc-prefix P] [--min X] [--top N]\n");
    return 2;
  }
  std::string url, model = "model", queryPrefix, docPrefix;
  double minimum = 0.45;
  size_t top = 3;
  for (int i = 3; i + 1 < argc; i += 2) {
    std::string flag = argv[i], value = argv[i + 1];
    if (flag == "--url") url = value;
    else if (flag == "--model") model = value;
    else if (flag == "--query-prefix") queryPrefix = value;
    else if (flag == "--doc-prefix") docPrefix = value;
    else if (flag == "--min") minimum = std::atof(value.c_str());
    else if (flag == "--top") top = (size_t)std::atoi(value.c_str());
  }
  ProjectIndex index;
  if (!url.empty()) index.useEmbeddings(model);
  index.update(argv[1]);
  std::atomic<bool> never{false};
  if (!url.empty()) {
    auto jobs = index.missing();
    for (size_t first = 0; first < jobs.size(); first += 8) {
      std::vector<std::string> texts;
      size_t last = std::min(jobs.size(), first + 8);
      for (size_t i = first; i < last; i++) texts.push_back(docPrefix + jobs[i].second.substr(0, 3000));
      std::vector<std::vector<float>> vectors;
      std::string error;
      if (!requestEmbeddings(url, texts, &vectors, &error, never, 120)) {
        std::fprintf(stderr, "embedding failed: %s\n", error.c_str());
        return 1;
      }
      for (size_t i = first; i < last; i++) index.setVector(jobs[i].first, vectors[i - first]);
    }
    std::fprintf(stderr, "%zu chunks, %zu embedded\n", index.chunks(), index.embedded());
  }
  std::ifstream queries(argv[2]);
  std::string query;
  while (std::getline(queries, query)) {
    if (query.empty()) continue;
    nlohmann::json out = {{"query", query}, {"keywords", hitsJson(index.search(query, top))}};
    if (!url.empty()) {
      std::vector<std::vector<float>> vectors;
      std::string error;
      if (requestEmbeddings(url, {queryPrefix + query}, &vectors, &error, never, 30) && !vectors.empty()) {
        out["both"] = hitsJson(index.search(query, top, "", &vectors[0], minimum));
        // the meaning alone, to see what each part contributes
        out["meaning"] = hitsJson(index.search("", top, "", &vectors[0], minimum));
      }
    }
    std::printf("%s\n", out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
  }
  return 0;
}
