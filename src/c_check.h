// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Checking a C file while it is edited: the compiler's own errors (undeclared names, assigning
// to a #define, wrong types...) before any build.
//
// The flags are the project's: `make -n -B` prints the commands its Makefile would run, and the
// one that compiles the file (or, for a header, the first C compile) gives the compiler, the
// include folders, the defines and the CPU. The editor's text is written to a copy in a
// temporary folder and checked with -fsyntax-only; -iquote keeps "header.h" next to the
// original working.
#pragma once
#include <string>
#include <vector>

#include "builder_parse.h"

namespace vcs {

struct CompileCommand {
  std::string compiler;            // as the Makefile names it (arm-none-eabi-gcc)
  std::vector<std::string> flags;  // only what changes how the file is read: -I, -D, -m, -std, -f, -W...
  bool empty() const { return compiler.empty(); }
};

// The folder make runs its Makefile in: workDir, changed by its -C options ("make -C elf").
std::string makeFolder(const std::vector<std::string>& makeArgs, const std::string& workDir);

// The command for source (absolute) in the dry run of a Makefile run in makeDir (its paths are
// relative to it, or to the folder a recursive make says it enters).
CompileCommand compileCommandFor(const std::string& dryRunOutput, const std::string& makeDir, const std::string& source);

// The arguments that check checkedCopy (the editor's text) as if it were at original.
std::vector<std::string> syntaxCheckArgs(const CompileCommand& command, const std::string& checkedCopy, const std::string& original);

// The compiler's errors and warnings in the checked copy, with file set to shownFile.
std::vector<Diagnostic> syntaxCheckDiagnostics(const std::string& output, const std::string& checkedCopy, const std::string& shownFile);

}  // namespace vcs
