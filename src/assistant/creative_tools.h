// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The agent's tools for the graphics and music editors (docs/design-assistant.md, "Tools for graphics and music"): a model
// cannot draw with a mouse or play a keyboard, but it can write a sprite as rows of X and ., a playfield as rows of 20 or
// 40 characters and a melody as note names. Each tool reads the .vcsgfx or .vcsmusic file (as the editor holds it, if it is
// open), changes one thing, checks the result against what the TIA and the project's kernel can do, and proposes the new file:
// nothing is written until the user accepts it, and then it lands in the editor as one undoable step. The *_code tools give
// the tables the editors export, so the program can use what was made.
#pragma once
#include <string>

#include "assistant/tools.h"

namespace vcs::assistant {

// True for the names this file handles.
bool isCreativeTool(const std::string& name);
ToolResult runCreativeTool(const ToolEnv& env, const ToolCall& call);

// Is the question about graphics or sound (or asked with a .vcsgfx or .vcsmusic file in front)? Then these tools are offered.
bool wantsCreativeTools(const std::string& question, const std::string& openFile);

// A note written by a model ("C4", "f#3", "Bb2", 60) as a MIDI note; false if it is not one.
bool parseNoteName(const std::string& text, int* midi);

}  // namespace vcs::assistant
