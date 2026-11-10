// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// A small HTTP/1.1 client for the assistant's worker threads (never the UI thread).
// Plain http:// only for now, which is what a local model server on this machine needs
// (phase 1 of docs/design-assistant.md). https:// will use each system's own TLS.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace vcs::assistant {

struct Url {
  std::string scheme, host, port, path;  // path includes the query, starts with '/'
  bool valid = false;
};
Url parseUrl(const std::string& url);
// "localhost", "127.x.x.x" and "::1" are local; anything else is remote.
bool isLoopbackHost(const std::string& host);

struct HttpRequest {
  std::string method = "GET";
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  int connectSeconds = 5;
  int idleSeconds = 120;  // no data for this long: give up (a model may think a while)
};

struct HttpResult {
  int status = 0;       // 0: no response
  std::string error;    // transport problem, or a refusal (https, bad URL)
  std::string errorBody;  // the body of a non-2xx answer, up to 4 KB
};

// Sends the request and calls onBody for each piece of the response body (chunked
// transfer already decoded), only for 2xx answers. onBody returns false to stop.
// cancel is checked about every 100 ms.
HttpResult httpRequest(const HttpRequest& request, const std::function<bool(const char*, size_t)>& onBody,
                       const std::atomic<bool>& cancel);

// A TCP port on 127.0.0.1 that is free right now (0 if none could be found).
int freeLoopbackPort();

// Decodes HTTP chunked transfer coding incrementally; exposed for tests.
class ChunkedDecoder {
 public:
  // Returns false once the final chunk has been seen or the data is malformed.
  bool feed(const char* bytes, size_t size, std::string& out);
  bool failed() const { return failed_; }
  bool done() const { return done_; }

 private:
  enum class State { Size, Data, DataEnd, Trailer } state_ = State::Size;
  std::string sizeLine_;
  size_t remaining_ = 0;
  bool failed_ = false, done_ = false;
};

}  // namespace vcs::assistant
