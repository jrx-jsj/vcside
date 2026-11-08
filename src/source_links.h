// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// File names in source lines that the editor can open with Ctrl/Cmd+click.
#pragma once
#include <string>

namespace vcs {

// The file a line names, as written: DASM's include and incbin, C's #include, batari
// Basic's include, inline and incgraphic, and the .vcsgfx of a `rem vcsgfx` block.
// start/end: its bytes in the line (without quotes). False if the line names none.
bool fileNameInLine(const std::string& line, std::string* name, int* start, int* end);

}  // namespace vcs
