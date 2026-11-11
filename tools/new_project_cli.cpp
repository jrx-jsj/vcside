// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Creates a project from a template, as New project does, for checking templates by hand:
//   vcside_new <folder> <name> <language: dasm, bb, elf-c, ace-c> <tv: NTSC, PAL, PAL60, AUTO> [example]
// It takes the templates from the data folder next to the executable (or the source tree's).
#include <cstdio>
#include <string>

#include "project.h"

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: vcside_new <folder> <name> <dasm|bb|elf-c|ace-c> <NTSC|PAL|PAL60|AUTO> [example]\n");
    return 2;
  }
  vcs::Language language = vcs::languageFromId(argv[3]);
  if (language == vcs::Language::Unknown) {
    std::fprintf(stderr, "vcside_new: unknown language %s\n", argv[3]);
    return 2;
  }
  vcs::Project project;
  std::string error;
  if (!vcs::Project::create(argv[1], argv[2], language, argv[4], &project, &error, argc > 5 ? argv[5] : "")) {
    std::fprintf(stderr, "vcside_new: %s\n", error.c_str());
    return 1;
  }
  std::printf("%s\n", project.file.c_str());
  return 0;
}
