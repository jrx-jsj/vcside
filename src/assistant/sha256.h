// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// SHA-256 (FIPS 180-4), to check a downloaded model against the hash the catalogue pins.
#pragma once
#include <cstdint>
#include <string>

namespace vcs::assistant {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t size);
  std::string hexDigest();  // call once, after the last update

 private:
  void block(const uint8_t* chunk);
  uint32_t state_[8];
  uint8_t pending_[64];
  size_t pendingSize_ = 0;
  uint64_t totalBytes_ = 0;
};

// The hash of a file, or "" if it cannot be read. stopFlag (optional) aborts early (returns "").
std::string sha256OfFile(const std::string& path, const bool* stopFlag = nullptr);

}  // namespace vcs::assistant
