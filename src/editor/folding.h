// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Regions of a source file that the editor can fold, found from its text.
#pragma once
#include <string>
#include <vector>

namespace vcs {

struct FoldRegion {
  int first = 0, last = 0;  // 0-based lines; the first stays visible when folded
};

enum class FoldSyntax { None, Asm, Basic, C };

// The regions, sorted by first line; one per first line (the largest), none of a single line.
//  Asm (DASM): MAC/MACRO..ENDM, IF*..ENDIF/EIF, REPEAT..REPEND, SEG and SUBROUTINE sections,
//    a label at column 0 to the next one, runs of two or more comment lines.
//  Basic (batari Basic): function/data/asm..end, blocks such as player0: or playfield: ..end,
//    rem vcsgfx..rem vcsgfx end, a label at column 0 to the next one, runs of rem lines.
//  C: braces and #if..#endif that span lines, block comments, runs of // lines.
// A region never takes the blank lines that end it.
std::vector<FoldRegion> foldRegions(const std::vector<std::string>& lines, FoldSyntax syntax);

}  // namespace vcs
