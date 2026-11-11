// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "pluscart_server.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "i18n.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static const SocketHandle kInvalidSocket = INVALID_SOCKET;
static void closeSocket(SocketHandle socketHandle) { closesocket(socketHandle); }
static void startSockets() {
  static bool started = false;
  if (!started) {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    started = true;
  }
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static const SocketHandle kInvalidSocket = -1;
static void closeSocket(SocketHandle socketHandle) { close(socketHandle); }
static void startSockets() {}
#endif

namespace vcs {

namespace {
// The PlusCart's menu entry types (MENU_ENTRY_Type in the firmware's global.h).
constexpr int kLeaveMenu = 0, kCartFile = 2;
constexpr long kMaxRange = 32768;  // the firmware asks for at most this much at a time

std::string lower(std::string text) {
  for (auto& character : text) character = (char)std::tolower((unsigned char)character);
  return text;
}

std::string readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

const ServedRom* findRom(const std::vector<ServedRom>& roms, const std::string& path) {
  std::string name = path;
  if (!name.empty() && name[0] == '/') name.erase(0, 1);
  for (auto& rom : roms)
    if (rom.name == name) return &rom;
  return nullptr;
}
}  // namespace

std::string decodePlusStorePath(const std::string& encoded) {
  std::string decoded;
  for (size_t index = 0; index < encoded.size(); index++) {
    char character = encoded[index];
    if (character == '+') {
      decoded += ' ';
    } else if (character == '%' && index + 2 < encoded.size() && std::isxdigit((unsigned char)encoded[index + 1]) &&
               std::isxdigit((unsigned char)encoded[index + 2])) {
      decoded += (char)std::stoi(encoded.substr(index + 1, 2), nullptr, 16);
      index += 2;
    } else {
      decoded += character;
    }
  }
  return decoded;
}

PlusStoreRequest parsePlusStoreRequest(const std::string& header) {
  PlusStoreRequest request;
  size_t lineEnd = header.find('\n');
  std::string requestLine = header.substr(0, lineEnd);
  const std::string prefix = "GET /api.php?p=";
  if (requestLine.compare(0, prefix.size(), prefix) != 0) return request;
  size_t pathEnd = requestLine.find(' ', prefix.size());
  if (pathEnd == std::string::npos) return request;
  request.valid = true;
  request.path = decodePlusStorePath(requestLine.substr(prefix.size(), pathEnd - prefix.size()));
  std::istringstream lines(header.substr(lineEnd == std::string::npos ? header.size() : lineEnd + 1));
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (lower(line).compare(0, 13, "range: bytes=") != 0) continue;
    long first = 0, last = 0;
    if (std::sscanf(line.c_str() + 13, "%ld-%ld", &first, &last) == 2 && first >= 0 && last >= first) {
      request.hasRange = true;
      request.first = first;
      request.last = last;
    }
  }
  return request;
}

std::string plusStoreMenu(const std::vector<ServedRom>& roms, const std::string& path, const std::string& message) {
  // status line: '0' no PlusStore account, '0' path levels to drop, the message
  std::string menu = "00" + message + "\n";
  char line[64];
  if (path.empty()) {
    for (auto& rom : roms) {
      long size = (long)readFile(rom.path).size();
      std::snprintf(line, sizeof line, "%d %06ld ", kCartFile, std::min(size, 999999L));
      menu += line + rom.name.substr(0, 32) + "\n";
    }
  } else {
    std::snprintf(line, sizeof line, "%d %06d ..\n", kLeaveMenu, 0);
    menu += line;
  }
  return menu;
}

std::string plusStoreResponse(const PlusStoreRequest& request, const std::vector<ServedRom>& roms, const std::string& message,
                              std::string* logLine) {
  if (!request.valid) return "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
  if (!request.hasRange) {
    std::string menu = plusStoreMenu(roms, request.path, message);
    if (logLine && request.path.empty()) *logLine = T("PlusCart: it asked for its menu.");
    return "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(menu.size()) +
           "\r\nConnection: keep-alive\r\n\r\n" + menu;
  }
  const ServedRom* rom = findRom(roms, request.path);
  std::string contents = rom ? readFile(rom->path) : std::string();
  long size = (long)contents.size();
  if (!rom || request.first >= size) {
    if (logLine) *logLine = F("PlusCart: it asked for {0}, which is not being served.", {request.path});
    return "HTTP/1.0 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
  }
  long last = std::min({request.last, size - 1, request.first + kMaxRange - 1});
  if (logLine && request.first == 0) *logLine = F("PlusCart: sending {0} ({1} bytes).", {rom->name, std::to_string(size)});
  std::string body = contents.substr((size_t)request.first, (size_t)(last - request.first + 1));
  return "HTTP/1.0 206 Partial Content\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nContent-Range: bytes " + std::to_string(request.first) + "-" + std::to_string(last) + "/" + std::to_string(size) +
         "\r\nConnection: keep-alive\r\n\r\n" + body;
}

PlusCartServer::~PlusCartServer() { stop(); }

bool PlusCartServer::start(int port, std::string* error) {
  stop();
  startSockets();
  SocketHandle listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener == kInvalidSocket) {
    if (error) *error = std::strerror(errno);
    return false;
  }
  int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof reuse);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons((uint16_t)port);
  if (bind(listener, (sockaddr*)&address, sizeof address) != 0 || listen(listener, 4) != 0) {
    if (error) *error = std::strerror(errno);
    closeSocket(listener);
    return false;
  }
  socklen_t length = sizeof address;
  getsockname(listener, (sockaddr*)&address, &length);
  port_ = ntohs(address.sin_port);
  listener_ = (long long)listener;
  stopping_ = false;
  running_ = true;
  thread_ = std::thread([this] { serve(); });
  return true;
}

void PlusCartServer::stop() {
  if (!running_) return;
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
  closeSocket((SocketHandle)listener_);
  listener_ = -1;
  running_ = false;
}

void PlusCartServer::setRoms(const std::vector<ServedRom>& roms, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  roms_ = roms;
  message_ = message;
}

std::vector<ServedRom> PlusCartServer::roms() {
  std::lock_guard<std::mutex> lock(mutex_);
  return roms_;
}

std::vector<std::string> PlusCartServer::takeLog() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> lines;
  lines.swap(log_);
  return lines;
}

// Waits up to `milliseconds` for something to read; false on a timeout or when stopping.
static bool waitReadable(SocketHandle socketHandle, int milliseconds) {
#ifdef _WIN32
  WSAPOLLFD descriptor{socketHandle, POLLRDNORM, 0};
  return WSAPoll(&descriptor, 1, milliseconds) > 0;
#else
  pollfd descriptor{socketHandle, POLLIN, 0};
  return poll(&descriptor, 1, milliseconds) > 0;
#endif
}

void PlusCartServer::serve() {
  SocketHandle listener = (SocketHandle)listener_;
  while (!stopping_) {
    if (!waitReadable(listener, 200)) continue;
    SocketHandle client = accept(listener, nullptr, nullptr);
    if (client == kInvalidSocket) continue;
    handleConnection((long long)client);
    closeSocket(client);
  }
}

// One connection: requests one after another (keep-alive) until the PlusCart closes it,
// it is quiet for a minute, or the server stops.
void PlusCartServer::handleConnection(long long clientHandle) {
  SocketHandle client = (SocketHandle)clientHandle;
  std::string received;
  int quietMilliseconds = 0;
  char buffer[4096];
  while (!stopping_ && quietMilliseconds < 60000) {
    size_t headerEnd = received.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
      std::string header = received.substr(0, headerEnd + 4);
      received.erase(0, headerEnd + 4);
      std::string message, logLine;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        message = message_;
      }
      std::string response = plusStoreResponse(parsePlusStoreRequest(header), roms(), message, &logLine);
      if (!logLine.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_.push_back(logLine);
      }
      for (size_t sent = 0; sent < response.size();) {
        auto count = send(client, response.data() + sent, (int)(response.size() - sent), 0);
        if (count <= 0) return;
        sent += (size_t)count;
      }
      continue;
    }
    if (!waitReadable(client, 200)) {
      quietMilliseconds += 200;
      continue;
    }
    auto count = recv(client, buffer, sizeof buffer, 0);
    if (count <= 0) return;  // closed
    received.append(buffer, (size_t)count);
    quietMilliseconds = 0;
  }
}

std::string localNetworkAddress() {
  startSockets();
  // A UDP socket "connected" to an outside address sends nothing, but the system picks
  // the interface (and address) it would use.
  SocketHandle probe = socket(AF_INET, SOCK_DGRAM, 0);
  if (probe == kInvalidSocket) return "";
  sockaddr_in outside{};
  outside.sin_family = AF_INET;
  outside.sin_port = htons(80);
  inet_pton(AF_INET, "192.0.2.1", &outside.sin_addr);  // TEST-NET-1: never reached
  std::string result;
  if (connect(probe, (sockaddr*)&outside, sizeof outside) == 0) {
    sockaddr_in local{};
    socklen_t length = sizeof local;
    char text[INET_ADDRSTRLEN] = {};
    if (getsockname(probe, (sockaddr*)&local, &length) == 0 && inet_ntop(AF_INET, &local.sin_addr, text, sizeof text)) result = text;
  }
  closeSocket(probe);
  return result == "0.0.0.0" ? "" : result;
}

}  // namespace vcs
