// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "c_check.h"

#include <filesystem>
#include <sstream>

#include "project.h"

namespace fs = std::filesystem;

namespace vcs {

static bool isCompiler(const std::string& word) {
  std::string name = fs::path(word).filename().string();
  for (const char* known : {"gcc", "g++", "clang", "clang++", "cc", "c++"}) {
    std::string suffix = known;
    if (name == suffix || (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 && name[name.size() - suffix.size() - 1] == '-'))
      return true;
  }
  return false;
}

static bool isSource(const std::string& word) {
  std::string extension = fs::path(word).extension().string();
  return extension == ".c" || extension == ".cpp" || extension == ".cc" || extension == ".cxx";
}

static std::string normalised(const std::string& projectDir, const std::string& path) {
  fs::path full(path);
  if (!full.is_absolute()) full = fs::path(projectDir) / full;
  return full.lexically_normal().generic_string();
}

// Keeps the flags that change how a file is read; drops what a compile or link does with it.
static std::vector<std::string> readingFlags(const std::vector<std::string>& words, const std::string& projectDir) {
  std::vector<std::string> flags;
  for (size_t index = 1; index < words.size(); index++) {
    const std::string& word = words[index];
    auto withPath = [&](const std::string& flag) -> bool {  // -I dir or -Idir: made absolute, as the check runs elsewhere
      if (word.compare(0, flag.size(), flag) != 0) return false;
      std::string path = word.size() > flag.size() ? word.substr(flag.size()) : index + 1 < words.size() ? words[++index] : "";
      if (!path.empty()) flags.push_back(flag), flags.push_back(normalised(projectDir, path));
      return true;
    };
    if (withPath("-isystem") || withPath("-iquote") || withPath("-include") || withPath("-I")) continue;
    if ((word == "-D" || word == "-U") && index + 1 < words.size()) {
      flags.push_back(word + words[++index]);
      continue;
    }
    if (word.compare(0, 4, "-Wl,") == 0 || word.compare(0, 4, "-Wa,") == 0 || word.compare(0, 4, "-Wp,") == 0) continue;
    for (const char* kept : {"-D", "-U", "-m", "-std=", "-f", "-W", "-O", "-ansi", "-pedantic"})
      if (word.compare(0, std::string(kept).size(), kept) == 0) {
        flags.push_back(word);
        break;
      }
  }
  return flags;
}

std::string makeFolder(const std::vector<std::string>& makeArgs, const std::string& workDir) {
  fs::path folder(workDir);
  for (size_t index = 1; index < makeArgs.size(); index++) {
    const std::string& word = makeArgs[index];
    std::string change;
    if (word == "-C" || word == "--directory") change = index + 1 < makeArgs.size() ? makeArgs[++index] : "";
    else if (word.compare(0, 2, "-C") == 0) change = word.substr(2);
    else if (word.compare(0, 12, "--directory=") == 0) change = word.substr(12);
    if (!change.empty()) folder = fs::path(change).is_absolute() ? fs::path(change) : folder / change;
  }
  return folder.lexically_normal().generic_string();
}

CompileCommand compileCommandFor(const std::string& dryRunOutput, const std::string& makeDir, const std::string& source) {
  std::string wanted = normalised(makeDir, source);
  CompileCommand firstCompile, found;
  // a recursive make says where it runs: "make[1]: Entering directory '/p/elf'" (` in old makes)
  std::vector<std::string> folders = {makeDir};
  std::istringstream in(dryRunOutput);
  for (std::string line; std::getline(in, line) && found.empty();) {
    if (line.rfind("make", 0) == 0 && line.find(": Entering directory ") != std::string::npos) {
      size_t start = line.find_first_of("'`", line.find(": Entering directory "));
      size_t end = line.rfind('\'');
      if (start != std::string::npos && end != std::string::npos && end > start) folders.push_back(line.substr(start + 1, end - start - 1));
      continue;
    }
    if (line.rfind("make", 0) == 0 && line.find(": Leaving directory ") != std::string::npos) {
      if (folders.size() > 1) folders.pop_back();
      continue;
    }
    const std::string& folder = folders.back();
    std::vector<std::string> words = splitCommand(line);
    if (words.empty() || !isCompiler(words[0])) continue;
    bool compilesSource = false, compilesWanted = false;
    for (auto& word : words) {
      if (!isSource(word)) continue;
      compilesSource = true;
      compilesWanted = compilesWanted || normalised(folder, word) == wanted;
    }
    if (!compilesSource) continue;
    CompileCommand command{words[0], readingFlags(words, folder)};
    if (compilesWanted) found = command;
    else if (firstCompile.empty()) firstCompile = command;
  }
  return found.empty() ? firstCompile : found;
}

std::vector<std::string> syntaxCheckArgs(const CompileCommand& command, const std::string& checkedCopy, const std::string& original) {
  std::vector<std::string> args = {command.compiler};
  args.insert(args.end(), command.flags.begin(), command.flags.end());
  std::string extension = fs::path(original).extension().string();
  bool cplusplus = extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".hpp";
  // the copy's own folder is not the original's: "header.h" is looked for next to the original
  args.insert(args.end(), {"-iquote", fs::path(original).parent_path().generic_string(), "-fsyntax-only", "-fdiagnostics-color=never",
                           "-x", cplusplus ? "c++" : "c", checkedCopy});
  return args;
}

std::vector<Diagnostic> syntaxCheckDiagnostics(const std::string& output, const std::string& checkedCopy, const std::string& shownFile) {
  std::vector<Diagnostic> diagnostics;
  std::string copy = fs::path(checkedCopy).lexically_normal().generic_string();
  for (auto& diagnostic : parseGccOutput(output)) {
    if (diagnostic.line <= 0 || diagnostic.severity == Diagnostic::Info) continue;  // notes only repeat what an error says
    if (fs::path(diagnostic.file).lexically_normal().generic_string() != copy) continue;  // a header's own problems: its own check
    diagnostic.file = shownFile;
    diagnostics.push_back(diagnostic);
  }
  return diagnostics;
}

}  // namespace vcs
