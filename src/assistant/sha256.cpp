// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/sha256.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vcs::assistant {

namespace {
const uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotateRight(uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }
}  // namespace

Sha256::Sha256() {
  static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(state_, initial, sizeof state_);
}

void Sha256::block(const uint8_t* chunk) {
  uint32_t words[64];
  for (int i = 0; i < 16; i++)
    words[i] = (uint32_t)chunk[i * 4] << 24 | (uint32_t)chunk[i * 4 + 1] << 16 | (uint32_t)chunk[i * 4 + 2] << 8 | chunk[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
    uint32_t s1 = rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; i++) {
    uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
    uint32_t choose = (e & f) ^ (~e & g);
    uint32_t temp1 = h + sum1 + choose + kRoundConstants[i] + words[i];
    uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = sum0 + majority;
    h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
  }
  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
  state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  totalBytes_ += size;
  while (size > 0) {
    size_t take = std::min(size, sizeof pending_ - pendingSize_);
    std::memcpy(pending_ + pendingSize_, bytes, take);
    pendingSize_ += take;
    bytes += take;
    size -= take;
    if (pendingSize_ == sizeof pending_) {
      block(pending_);
      pendingSize_ = 0;
    }
  }
}

std::string Sha256::hexDigest() {
  uint64_t bitLength = totalBytes_ * 8;
  uint8_t padding[72] = {0x80};
  size_t paddingSize = (pendingSize_ < 56 ? 56 : 120) - pendingSize_;
  for (int i = 0; i < 8; i++) padding[paddingSize + i] = (uint8_t)(bitLength >> (56 - 8 * i));
  update(padding, paddingSize + 8);
  std::string hex;
  char piece[9];
  for (uint32_t word : state_) {
    std::snprintf(piece, sizeof piece, "%08x", word);
    hex += piece;
  }
  return hex;
}

std::string sha256OfFile(const std::string& path, const bool* stopFlag) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return "";
  Sha256 hash;
  std::vector<uint8_t> buffer(1 << 20);
  size_t count;
  while ((count = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
    if (stopFlag && *stopFlag) {
      std::fclose(file);
      return "";
    }
    hash.update(buffer.data(), count);
  }
  std::fclose(file);
  return hash.hexDigest();
}

}  // namespace vcs::assistant
