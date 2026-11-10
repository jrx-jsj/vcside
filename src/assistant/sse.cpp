// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/sse.h"

namespace vcs::assistant {

void SseParser::endOfLine(std::vector<std::string>& events) {
  if (line_.empty()) {  // a blank line ends the event
    if (hasData_) events.push_back(data_);
    data_.clear();
    hasData_ = false;
    return;
  }
  if (line_[0] != ':') {
    std::string field = line_, value;
    size_t colon = line_.find(':');
    if (colon != std::string::npos) {
      field = line_.substr(0, colon);
      value = line_.substr(colon + 1);
      if (!value.empty() && value[0] == ' ') value.erase(0, 1);
    }
    if (field == "data") {
      if (hasData_) data_ += '\n';
      data_ += value;
      hasData_ = true;
    }
  }
  line_.clear();
}

std::vector<std::string> SseParser::feed(const char* bytes, size_t size) {
  std::vector<std::string> events;
  for (size_t i = 0; i < size; i++) {
    char c = bytes[i];
    if (pendingCr_) {
      pendingCr_ = false;
      if (c == '\n') continue;
    }
    if (c == '\r' || c == '\n') {
      pendingCr_ = c == '\r';
      endOfLine(events);
      line_.clear();
    } else {
      line_ += c;
    }
  }
  return events;
}

}  // namespace vcs::assistant
