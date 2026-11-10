// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Server-sent events (WHATWG HTML Standard, "Server-sent events"): the stream format
// both chat dialects answer with. Fed with whatever chunks the network delivers.
#pragma once
#include <string>
#include <vector>

namespace vcs::assistant {

class SseParser {
 public:
  // Adds received bytes; returns the "data" payload of each event completed by them
  // (several data lines of one event are joined with '\n'). Comments and the other
  // fields (event, id, retry) are ignored: neither chat dialect needs them.
  std::vector<std::string> feed(const char* bytes, size_t size);

 private:
  std::string line_;
  std::string data_;
  bool hasData_ = false;
  bool pendingCr_ = false;  // the last byte was '\r': a following '\n' belongs to it
  void endOfLine(std::vector<std::string>& events);
};

}  // namespace vcs::assistant
