// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/http.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static const SocketHandle kInvalidSocket = INVALID_SOCKET;
static void closeSocket(SocketHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static const SocketHandle kInvalidSocket = -1;
static void closeSocket(SocketHandle s) { close(s); }
#endif

namespace vcs::assistant {

namespace {
std::string lower(std::string text) {
  for (auto& c : text) c = (char)std::tolower((unsigned char)c);
  return text;
}
}  // namespace

Url parseUrl(const std::string& url) {
  Url result;
  size_t scheme = url.find("://");
  if (scheme == std::string::npos) return result;
  result.scheme = lower(url.substr(0, scheme));
  if (result.scheme != "http" && result.scheme != "https") return result;
  std::string rest = url.substr(scheme + 3);
  size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  result.path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (authority.find('@') != std::string::npos) return result;  // no credentials in URLs
  if (!authority.empty() && authority[0] == '[') {              // [::1]:8080
    size_t close = authority.find(']');
    if (close == std::string::npos) return result;
    result.host = authority.substr(1, close - 1);
    if (close + 1 < authority.size() && authority[close + 1] == ':') result.port = authority.substr(close + 2);
  } else {
    size_t colon = authority.rfind(':');
    result.host = colon == std::string::npos ? authority : authority.substr(0, colon);
    if (colon != std::string::npos) result.port = authority.substr(colon + 1);
  }
  if (result.port.empty()) result.port = result.scheme == "https" ? "443" : "80";
  result.valid = !result.host.empty();
  return result;
}

bool isLoopbackHost(const std::string& host) {
  std::string name = lower(host);
  if (name == "localhost" || name == "::1") return true;
  // 127.x.y.z, digits and dots only: "127.example.com" is a name that resolves elsewhere
  if (name.rfind("127.", 0) != 0) return false;
  int dots = 0;
  for (char c : name) {
    if (c == '.') dots++;
    else if (!std::isdigit((unsigned char)c)) return false;
  }
  return dots == 3;
}

bool ChunkedDecoder::feed(const char* bytes, size_t size, std::string& out) {
  for (size_t i = 0; i < size && !done_ && !failed_; i++) {
    char c = bytes[i];
    switch (state_) {
      case State::Size:
        if (c == '\n') {
          std::string digits = sizeLine_.substr(0, sizeLine_.find(';'));  // drop extensions
          while (!digits.empty() && (digits.back() == '\r' || digits.back() == ' ')) digits.pop_back();
          if (digits.empty() || digits.size() > 8 || digits.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
            failed_ = true;
            break;
          }
          remaining_ = std::stoul(digits, nullptr, 16);
          sizeLine_.clear();
          state_ = remaining_ == 0 ? State::Trailer : State::Data;
        } else {
          sizeLine_ += c;
          if (sizeLine_.size() > 64) failed_ = true;
        }
        break;
      case State::Data: {
        size_t take = std::min(remaining_, size - i);
        out.append(bytes + i, take);
        remaining_ -= take;
        i += take - 1;
        if (remaining_ == 0) state_ = State::DataEnd;
        break;
      }
      case State::DataEnd:  // the CRLF after the data
        if (c == '\n') state_ = State::Size;
        else if (c != '\r') failed_ = true;
        break;
      case State::Trailer:  // after the last chunk: trailer lines up to a blank one
        if (c == '\n') {
          if (sizeLine_.empty() || sizeLine_ == "\r") done_ = true;
          sizeLine_.clear();
        } else {
          sizeLine_ += c;
        }
        break;
    }
  }
  return !done_ && !failed_;
}

namespace {

struct Socket {
  SocketHandle handle = kInvalidSocket;
  ~Socket() {
    if (handle != kInvalidSocket) closeSocket(handle);
  }
};

#ifdef _WIN32
struct WinsockStart {
  WinsockStart() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
};
#endif

// Waits up to milliseconds for the socket to be readable (true) or writable.
bool waitSocket(SocketHandle handle, bool forWrite, int milliseconds) {
#ifdef _WIN32
  WSAPOLLFD entry{handle, (SHORT)(forWrite ? POLLWRNORM : POLLRDNORM), 0};
  return WSAPoll(&entry, 1, milliseconds) > 0;
#else
  pollfd entry{handle, (short)(forWrite ? POLLOUT : POLLIN), 0};
  return poll(&entry, 1, milliseconds) > 0;
#endif
}

bool sendAll(SocketHandle handle, const std::string& data, const std::atomic<bool>& cancel) {
  size_t sent = 0;
  while (sent < data.size()) {
    if (cancel) return false;
    if (!waitSocket(handle, true, 100)) continue;
    int n = (int)send(handle, data.data() + sent, (int)std::min<size_t>(data.size() - sent, 1 << 16), 0);
    if (n <= 0) return false;
    sent += (size_t)n;
  }
  return true;
}

SocketHandle connectTo(const Url& url, int seconds, std::string* error) {
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* list = nullptr;
  if (getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &list) != 0 || !list) {
    *error = "cannot resolve " + url.host;
    return kInvalidSocket;
  }
  SocketHandle found = kInvalidSocket;
  for (addrinfo* a = list; a && found == kInvalidSocket; a = a->ai_next) {
    SocketHandle handle = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (handle == kInvalidSocket) continue;
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(handle, FIONBIO, &nonBlocking);
#else
    fcntl(handle, F_SETFL, fcntl(handle, F_GETFL, 0) | O_NONBLOCK);
#endif
    int rc = connect(handle, a->ai_addr, (int)a->ai_addrlen);
    bool ok = rc == 0;
    if (!ok) {
      if (waitSocket(handle, true, seconds * 1000)) {
        int soError = 0;
        socklen_t length = sizeof soError;
        getsockopt(handle, SOL_SOCKET, SO_ERROR, (char*)&soError, &length);
        ok = soError == 0;
      }
    }
    if (ok) found = handle;
    else closeSocket(handle);
  }
  freeaddrinfo(list);
  if (found == kInvalidSocket) *error = "cannot connect to " + url.host + ":" + url.port;
  return found;
}

}  // namespace

static HttpsBackend httpsBackend_ = nullptr;
void setHttpsBackend(HttpsBackend backend) { httpsBackend_ = backend; }
bool httpsAvailable() { return httpsBackend_ != nullptr; }

HttpResult httpRequest(const HttpRequest& request, const std::function<bool(const char*, size_t)>& onBody,
                       const std::atomic<bool>& cancel) {
  HttpResult result;
#ifdef _WIN32
  static WinsockStart winsock;
#endif
  Url url = parseUrl(request.url);
  if (!url.valid) {
    result.error = "invalid URL";
    return result;
  }
  if (url.scheme == "https") {
    if (httpsBackend_) return httpsBackend_(request, onBody, cancel);
    result.error = "https is not available on this system yet: use a local model, or an http:// endpoint";
    return result;
  }
  Socket socket_;
  socket_.handle = connectTo(url, request.connectSeconds, &result.error);
  if (socket_.handle == kInvalidSocket) return result;

  std::string head = request.method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host +
                     (url.port == "80" ? "" : ":" + url.port) + "\r\nConnection: close\r\nAccept: */*\r\nUser-Agent: VCSIDE\r\n";
  for (auto& header : request.headers) head += header.first + ": " + header.second + "\r\n";
  if (!request.body.empty() || request.method == "POST") head += "Content-Length: " + std::to_string(request.body.size()) + "\r\n";
  head += "\r\n";
  if (!sendAll(socket_.handle, head + request.body, cancel)) {
    result.error = cancel ? "cancelled" : "send failed";
    return result;
  }

  std::string header;
  bool headerDone = false, chunked = false;
  long long contentLength = -1, received = 0;
  ChunkedDecoder chunks;
  auto lastData = std::chrono::steady_clock::now();
  char buffer[8192];
  std::string leftover;  // body bytes that arrived with the header
  for (;;) {
    if (cancel) {
      result.error = "cancelled";
      return result;
    }
    if (!waitSocket(socket_.handle, false, 100)) {
      if (std::chrono::steady_clock::now() - lastData > std::chrono::seconds(request.idleSeconds)) {
        result.error = "timed out waiting for the model";
        return result;
      }
      continue;
    }
    int n = (int)recv(socket_.handle, buffer, sizeof buffer, 0);
    if (n < 0) {
      result.error = "connection error";
      return result;
    }
    if (n == 0) break;  // closed: the end of a body with no length
    lastData = std::chrono::steady_clock::now();
    const char* bytes = buffer;
    size_t size = (size_t)n;
    if (!headerDone) {
      header.append(buffer, (size_t)n);
      size_t end = header.find("\r\n\r\n");
      if (end == std::string::npos) {
        if (header.size() > 64 * 1024) {
          result.error = "bad response";
          return result;
        }
        continue;
      }
      headerDone = true;
      std::string rest = header.substr(end + 4);
      header.resize(end);
      // status line, then headers
      if (header.size() < 12 || header.compare(0, 5, "HTTP/") != 0) {
        result.error = "bad response";
        return result;
      }
      result.status = std::atoi(header.c_str() + header.find(' ') + 1);
      std::string lowered = lower(header);
      chunked = lowered.find("transfer-encoding: chunked") != std::string::npos;
      size_t ra = lowered.find("\r\nretry-after:");
      if (ra != std::string::npos) result.retryAfterSeconds = std::atof(lowered.c_str() + ra + 14);
      size_t cl = lowered.find("\r\ncontent-length:");
      if (cl != std::string::npos) contentLength = std::atoll(lowered.c_str() + cl + 17);
      leftover = std::move(rest);
      bytes = leftover.data();
      size = leftover.size();
      if (size == 0) {
        if (contentLength == 0) break;
        continue;
      }
    }
    std::string decoded;
    bool more = true;
    if (chunked) {
      more = chunks.feed(bytes, size, decoded);
      if (chunks.failed()) {
        result.error = "bad chunked data";
        return result;
      }
    } else {
      decoded.assign(bytes, size);
    }
    received += (long long)decoded.size();
    if (!decoded.empty()) {
      if (result.status / 100 == 2) {
        if (!onBody(decoded.data(), decoded.size())) {
          result.error = "cancelled";
          return result;
        }
      } else if (result.errorBody.size() < 4096) {
        result.errorBody += decoded.substr(0, 4096 - result.errorBody.size());
      }
    }
    if (!more || (contentLength >= 0 && received >= contentLength)) break;
  }
  if (!headerDone) result.error = "no response";
  return result;
}

int freeLoopbackPort() {
#ifdef _WIN32
  static WinsockStart winsock;
#endif
  Socket socket_;
  socket_.handle = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_.handle == kInvalidSocket) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(socket_.handle, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) return 0;
  socklen_t length = sizeof address;
  if (getsockname(socket_.handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) return 0;
  return ntohs(address.sin_port);
}

}  // namespace vcs::assistant
