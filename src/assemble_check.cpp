// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assemble_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "child_process.h"

namespace vcs {

namespace fs = std::filesystem;

namespace {
bool skippedFolder(const std::string& name) { return name.empty() || name[0] == '.' || name == "build" || name == "bin" || name == "obj" || name == "out" || name == "node_modules"; }

// The sources a DASM build may read: text files of a sensible size. ROMs, listings and images stay behind.
bool copied(const fs::path& path) {
  std::string extension = path.extension().string();
  for (auto& c : extension) c = (char)std::tolower((unsigned char)c);
  for (const char* skip : {".bin", ".a26", ".rom", ".lst", ".sym", ".png", ".bmp", ".jpg", ".gif", ".wav", ".elf", ".o", ".zip", ".ace"})
    if (extension == skip) return false;
  std::error_code code;
  return fs::file_size(path, code) <= (2u << 20) && !code;
}
}  // namespace

AssembleCheck assembleWith(const Project& project, Toolchain& toolchain, const std::string& relative, const std::string& text, int timeoutSeconds) {
  AssembleCheck check;
  if (project.language != Language::Dasm || !project.buildCommand.empty() || project.dir.empty()) return check;
  std::error_code code;
  fs::path scratch = fs::temp_directory_path(code) / ("vcside-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(scratch, code);
  if (code) return check;
  struct Cleanup {
    fs::path folder;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(folder, ignored);
    }
  } cleanup{scratch};
  int files = 0;
  fs::recursive_directory_iterator it(project.dir, fs::directory_options::skip_permission_denied, code), end;
  for (; it != end && files < 2000; it.increment(code)) {
    if (code) break;
    std::string name = it->path().filename().string();
    if (it->is_directory(code)) {
      if (skippedFolder(name) || it.depth() >= 5) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(code) || !copied(it->path())) continue;
    fs::path target = scratch / fs::relative(it->path(), project.dir, code);
    fs::create_directories(target.parent_path(), code);
    fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, code);
    files++;
  }
  fs::path changed = scratch / fs::path(relative);
  fs::create_directories(changed.parent_path(), code);
  {
    std::ofstream out(changed, std::ios::binary);
    out << text;
    if (!out) return check;
  }
  Project copy = project;
  copy.dir = scratch.string();
  copy.file = (scratch / fs::path(project.file).filename()).string();
  std::string error, workDir;
  EnvVars environment;
  std::vector<std::string> arguments = copy.buildArgs(toolchain, &error, &workDir, &environment);
  if (arguments.empty()) return check;  // no assembler here: nothing to say
  fs::create_directories(fs::path(copy.outputPath()).parent_path(), code);
  Job job;
  if (!job.start(arguments, workDir.empty() ? copy.dir : workDir, environment)) return check;
  auto started = std::chrono::steady_clock::now();
  while (job.running()) {
    if (std::chrono::steady_clock::now() - started > std::chrono::seconds(timeoutSeconds)) {
      job.stopAndWait();
      return check;  // took too long: not judged
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  check.judged = true;
  std::string output = job.output();
  // the temporary folder's name says nothing to anyone: paths as the project has them
  std::string prefix = scratch.string() + std::string(1, fs::path::preferred_separator);
  for (size_t at = output.find(prefix); at != std::string::npos; at = output.find(prefix, at)) output.erase(at, prefix.size());
  std::string kept;
  int lines = 0;
  bool unresolved = false;
  std::istringstream in(output);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string lower = line;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower.rfind("> ", 0) == 0) continue;  // the command line Job puts first
    if (lower.find("unresolved symbol list") != std::string::npos) unresolved = true;
    bool wanted = lower.find("error") != std::string::npos || lower.find("unresolved") != std::string::npos || lower.find("fatal") != std::string::npos ||
                  lower.find("unknown mnemonic") != std::string::npos || lower.find("out of range") != std::string::npos || lower.find("abort") != std::string::npos ||
                  (unresolved && !line.empty() && line.find("0000 ????") != std::string::npos);
    if (!wanted || lines >= 12) continue;
    kept += line + "\n";
    lines++;
  }
  check.ok = job.exitCode() == 0 && kept.empty();
  if (!check.ok) check.errors = kept.empty() ? output.substr(output.size() > 600 ? output.size() - 600 : 0) : kept;
  return check;
}

}  // namespace vcs
