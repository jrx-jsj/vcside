// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The assembler as a judge of a change that has not been made yet: the project's sources are copied to a temporary folder, one
// file gets its proposed text and the copy is built. What the assembler says goes back to whoever proposed the change (the
// assistant's model) before the user is asked to look at it. Only for projects DASM builds by itself: a Makefile or a script may do
// anything, and other toolchains take too long to run on every proposal.
#pragma once
#include <string>

#include "project.h"
#include "toolchain.h"

namespace vcs {

struct AssembleCheck {
  bool judged = false;  // false: this project cannot be judged this way (not DASM, no assembler, a build command of its own)
  bool ok = false;      // it assembled
  std::string errors;   // what the assembler said, a few lines, paths relative to the project
};

// relative: the changed file, relative to the project; text: its proposed content.
AssembleCheck assembleWith(const Project& project, Toolchain& toolchain, const std::string& relative, const std::string& text, int timeoutSeconds = 10);

}  // namespace vcs
