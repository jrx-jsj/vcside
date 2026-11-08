// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VCSIDE contributors
// Sizes of a cartridge format used by the space warnings (formats.json "space").
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace vcs {

struct Range {
  long lo = 0, hi = 0;  // [lo, hi)
  long size() const { return hi - lo; }
};

struct FormatSpace {
  bool known = false;
  std::vector<long> romSizes;      // valid image sizes (empty = any)
  long bankSize = 4096;
  std::vector<Range> reserved;     // ranges in a bank's window ($1000-$1FFF) not for code/data
  long ramBytes = 0;               // cartridge RAM
  std::vector<Range> ramWindows;   // windows through which it is declared (empty = not measurable)
};

// "$1FF8-$1FF9" -> [0x1FF8, 0x1FFA)
std::optional<Range> parseRange(const std::string& s);

}  // namespace vcs
