// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Runs the assistant's guided procedure outside the IDE, against a model server, to measure it (tools/assistant_eval/guided_run.py):
//   vcside_guided <project.vcsproj> <file, relative> "<request>" --url <server /v1> [--model ID] [--kb DIR] [--out FILE] [--no-assemble]
// Prints one JSON object: the steps (question label, the model's answer, seconds), the outcome, the message, the lines added and
// whether the project assembles with them. --out gets the file's new text.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "assemble_check.h"
#include "assistant/domain_checks.h"
#include "assistant/guided.h"
#include "assistant/one_shot.h"
#include "assistant/project_index.h"
#include "kb.h"
#include "project.h"
#include "toolchain.h"

using namespace vcs;
using namespace vcs::assistant;
namespace fs = std::filesystem;

static std::string readAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: vcside_guided <project.vcsproj> <file> \"<request>\" --url U [--model ID] [--kb DIR] [--out FILE] [--no-assemble]\n");
    return 2;
  }
  std::string url, model = "model", kbDir = "data/kb", outFile;
  bool assemble = true;
  for (int i = 4; i < argc; i++) {
    std::string flag = argv[i];
    if (flag == "--no-assemble") assemble = false;
    else if (i + 1 < argc && flag == "--url") url = argv[++i];
    else if (i + 1 < argc && flag == "--model") model = argv[++i];
    else if (i + 1 < argc && flag == "--kb") kbDir = argv[++i];
    else if (i + 1 < argc && flag == "--out") outFile = argv[++i];
  }
  if (url.empty()) return std::fprintf(stderr, "--url is needed\n"), 2;
  std::string error;
  Project project;
  if (!project.load(argv[1], &error)) return std::fprintf(stderr, "project: %s\n", error.c_str()), 1;
  KnowledgeBase kb;
  if (!kb.load(kbDir, &error)) return std::fprintf(stderr, "knowledge base: %s\n", error.c_str()), 1;
  Toolchain toolchain;
  toolchain.detect();
  ProjectIndex index;
  index.update(project.dir);

  GuidedInput input;
  input.request = argv[3];
  input.projectDir = project.dir;
  input.file = argv[2];
  input.fileText = readAll(fs::path(project.dir) / input.file);
  input.notes = readAll(fs::path(project.dir) / "VCSIDE.md");
  input.kb = &kb;
  input.batariBasic = project.language == Language::BatariBasic;
  input.inProject = [&](const std::string& word) { return index.knows(word); };
  if (assemble)
    input.assemble = [&](const std::string& relative, const std::string& text, std::string* errors) {
      AssembleCheck check = assembleWith(project, toolchain, relative, text);
      if (!check.judged) return false;
      *errors = check.ok ? std::string() : check.errors;
      return true;
    };

  Provider provider;
  provider.baseUrl = url, provider.model = model, provider.dialect = "openai";
  provider.builtin = true;  // as VCSIDE's own helper: no thinking, low temperature

  nlohmann::json report = {{"request", input.request}, {"file", input.file}, {"steps", nlohmann::json::array()}};
  Guided guided;
  GuidedStep step;
  std::string why;
  if (!guided.start(input, &step, &why)) {
    report["outcome"] = "not applicable", report["message"] = why;
    std::cout << report.dump(1) << "\n";
    return 1;
  }
  auto started = std::chrono::steady_clock::now();
  for (int turn = 0; turn < 8; turn++) {
    auto asked = std::chrono::steady_clock::now();
    std::string text;
    if (!chatOnce(provider, {{"system", step.system}, {"user", step.prompt}}, &step.schema, step.maxTokens, &text, &error, 120, step.temperature)) {
      report["outcome"] = "server error", report["message"] = error;
      std::cout << report.dump(1) << "\n";
      return 1;
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - asked).count();
    report["steps"].push_back({{"label", step.label}, {"answer", nlohmann::json::parse(text, nullptr, false).is_discarded() ? nlohmann::json(text) : nlohmann::json::parse(text)}, {"seconds", seconds}, {"prompt_chars", step.prompt.size()}});
    GuidedStep next;
    EditProposal proposal;
    std::string message;
    Guided::Outcome outcome = guided.answer(text, &next, &proposal, &message);
    if (outcome == Guided::Outcome::Ask) {
      step = next;
      continue;
    }
    report["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    report["recipes"] = guided.recipes(), report["routine"] = guided.routine(), report["attempts"] = guided.attempts();
    report["message"] = message;
    if (outcome == Guided::Outcome::Failed) {
      report["outcome"] = "failed";
      std::cout << report.dump(1) << "\n";
      return 1;
    }
    report["outcome"] = "proposal";
    {  // the lines put in (the block, and the variables declared above it)
      std::string block, declared = addedLines(proposal.before, proposal.after);
      std::istringstream lines(proposal.after);
      int number = 0;
      for (std::string line; std::getline(lines, line);)
        if (++number >= proposal.firstLine && number <= proposal.lastLine) block += line + "\n";
      std::istringstream extra(declared);
      for (std::string line; std::getline(extra, line);)
        if (block.find(line + "\n") == std::string::npos) block = line + "\n" + block;
      report["added"] = block;
    }
    report["first_line"] = proposal.firstLine, report["last_line"] = proposal.lastLine;
    if (assemble) {
      AssembleCheck check = assembleWith(project, toolchain, input.file, proposal.after);
      report["assembles"] = check.judged ? nlohmann::json(check.ok) : nlohmann::json(nullptr);
    }
    if (!outFile.empty()) std::ofstream(outFile, std::ios::binary) << proposal.after;
    std::cout << report.dump(1) << "\n";
    return 0;
  }
  report["outcome"] = "failed", report["message"] = "too many steps";
  std::cout << report.dump(1) << "\n";
  return 1;
}
