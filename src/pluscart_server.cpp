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
  std::istringstream headerLines(header.substr(lineEnd == std::string::npos ? header.size() : lineEnd + 1));
  for (std::string line; std::getline(headerLines, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (lower(line).compare(0, 5, "host:") == 0) request.host = line.substr(line.find_first_not_of(' ', 5) == std::string::npos ? line.size() : line.find_first_not_of(' ', 5));
  }
  const std::string prefix = "GET /api.php?p=";
  if (requestLine.compare(0, prefix.size(), prefix) != 0) {
    if (requestLine.compare(0, 5, "GET /") == 0) {
      size_t pathEnd = requestLine.find(' ', 4);
      request.browserPath = decodePlusStorePath(requestLine.substr(4, pathEnd == std::string::npos ? std::string::npos : pathEnd - 4));
      request.browserPath = request.browserPath.substr(0, request.browserPath.find('?'));
    }
    return request;
  }
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

namespace {
std::string escapeHtml(const std::string& text) {
  std::string escaped;
  for (char character : text) {
    switch (character) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '"': escaped += "&quot;"; break;
      default: escaped += character;
    }
  }
  return escaped;
}

std::string encodeUrlPath(const std::string& text) {
  std::string encoded;
  char hex[4];
  for (unsigned char character : text) {
    if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
      encoded += (char)character;
    } else {
      std::snprintf(hex, sizeof hex, "%%%02X", character);
      encoded += hex;
    }
  }
  return encoded;
}

std::string sizeText(long bytes) {
  char text[32];
  std::snprintf(text, sizeof text, "%.1f", bytes / 1024.0);
  return bytes < 1024 ? F("{0} bytes", {std::to_string(bytes)}) : F("{0} KB ({1} bytes)", {text, std::to_string(bytes)});
}

std::string httpAnswer(const std::string& status, const std::string& type, const std::string& body, const std::string& extra = "") {
  return "HTTP/1.0 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra +
         "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n" + body;
}
}  // namespace

// The website's look (vcside.com: a cartridge label on wood), with no outside files: the
// computer serving it may have no internet.
std::string plusCartLoaderPage(const std::vector<ServedRom>& roms, const std::string& message, const std::string& host) {
  std::string address = host.substr(0, host.find(':'));
  std::string page = R"(<!doctype html>
<html lang="LANG"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>VCSIDE PlusCart Loader</title>
<style>
:root { --ink: #141414; --paper: #f4ecdc; --line: #d9ccb2; --muted: #6b604f; --red: #d8402a; --orange: #ee7a22; --yellow: #f4b52c; --wood: #3b2416; }
* { box-sizing: border-box; }
body { margin: 0; color: var(--ink); background-color: var(--wood); font: 17px/1.6 system-ui, -apple-system, "Segoe UI", sans-serif;
  background-image: repeating-linear-gradient(92deg, rgba(255,255,255,.03) 0 3px, transparent 3px 11px), repeating-linear-gradient(88deg, rgba(0,0,0,.12) 0 2px, transparent 2px 23px); }
.wrap { max-width: 820px; margin: 24px auto; padding: 0 16px; }
.cart { background: #1b1b1b; border-radius: 10px 10px 4px 4px; padding: 14px 14px 0; box-shadow: 0 18px 40px rgba(0,0,0,.45), inset 0 2px 0 #333; }
.label { background: #0d0d0d; color: #fff; border-radius: 4px; overflow: hidden; }
.stripes { height: 14px; background: linear-gradient(to bottom, var(--yellow) 0 33.3%, var(--orange) 33.3% 66.6%, var(--red) 66.6% 100%); }
.top { padding: 22px 24px 18px; }
h1 { font: 900 clamp(40px, 9vw, 64px)/.95 system-ui, sans-serif; letter-spacing: -2px; margin: 0; }
.sub { font-weight: 700; text-transform: uppercase; letter-spacing: 3px; color: var(--yellow); margin: 6px 0 0; font-size: 14px; }
.ridges { height: 22px; margin: 14px -14px 0; background: repeating-linear-gradient(to right, #222 0 10px, #161616 10px 20px); border-radius: 0 0 4px 4px; }
.sheet { background: var(--paper); border-radius: 4px; padding: 28px clamp(18px, 4vw, 40px); box-shadow: 0 18px 40px rgba(0,0,0,.4); margin-top: 28px; }
.sheet h2 { font: 900 24px/1.2 system-ui, sans-serif; margin: 0 0 12px; }
.sheet h2 + p { margin-top: 0; }
.rom { display: flex; align-items: center; gap: 16px; flex-wrap: wrap; padding: 14px 0; border-top: 1px solid var(--line); }
.rom:last-of-type { border-bottom: 1px solid var(--line); }
.rom .name { font: 600 17px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; word-break: break-all; }
.rom .size { color: var(--muted); font-size: 15px; }
.rom .info { margin-right: auto; }
.btn { display: inline-block; background: var(--orange); color: var(--ink); font-weight: 800; text-decoration: none; padding: 8px 18px; border-radius: 3px; }
.btn:hover { background: var(--yellow); }
code { font: .9em ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; background: #00000010; padding: .08em .3em; border-radius: 3px; }
.muted { color: var(--muted); font-size: 15px; }
</style></head><body><div class="wrap">
<div class="cart"><div class="label"><div class="stripes"></div><div class="top"><h1>VCSIDE</h1><p class="sub">PlusCart Loader</p></div></div><div class="ridges"></div></div>
<div class="sheet">
)";
  page.replace(page.find("LANG"), 4, currentLanguage().empty() ? std::string("en") : currentLanguage());
  page += "<h2>" + escapeHtml(message.empty() ? std::string("VCSIDE") : message) + "</h2>\n";
  if (roms.empty()) {
    page += "<p>" + escapeHtml(T("No ROM yet: build the project in VCSIDE and reload this page.")) + "</p>\n";
  } else {
    page += "<p>" + escapeHtml(roms.size() == 1 ? T("The ROM VCSIDE serves now, as last built:") : T("The ROMs VCSIDE serves now, as last built:")) + "</p>\n";
    for (auto& rom : roms) {
      long size = (long)readFile(rom.path).size();
      page += "<div class=\"rom\"><div class=\"info\"><div class=\"name\">" + escapeHtml(rom.name) + "</div><div class=\"size\">" + sizeText(size) +
              "</div></div><a class=\"btn\" href=\"/rom/" + encodeUrlPath(rom.name) + "\" download>" + escapeHtml(T("Download")) + "</a></div>\n";
    }
  }
  page += "<h2 style=\"margin-top:28px\">" + escapeHtml(T("On the PlusCart")) + "</h2>\n<p>" +
          F(escapeHtml(T("In its menu, {0}: {1}. Its menu then lists the ROMs above, read again after every build. For the PlusStore again, set the API Host back to {2}.")).c_str(),
            {"<b>Setup &rarr; API Host</b>",
             "<code>" + escapeHtml(address.empty() ? std::string(T("this computer's address")) : address) + "</code>",
             "<code>pluscart.firmaplus.de</code>"}) +
          "</p>\n<p class=\"muted\">" + F(escapeHtml(T("Served by VCSIDE while {0} is on.")).c_str(), {"<i>" + escapeHtml(T("Build")) + " &rarr; " + escapeHtml(T("Serve to the PlusCart (Wi-Fi)")) + "</i>"}) +
          "</p>\n</div></div></body></html>\n";
  return page;
}

std::string plusStoreResponse(const PlusStoreRequest& request, const std::vector<ServedRom>& roms, const std::string& message,
                              std::string* logLine) {
  if (!request.valid && !request.browserPath.empty()) {  // a browser
    if (request.browserPath == "/" || request.browserPath == "/index.html")
    {
      // opened on this computer as localhost: the PlusCart needs the address on the network
      std::string host = request.host.substr(0, request.host.rfind(':') == std::string::npos || request.host.back() == ']' ? std::string::npos : request.host.rfind(':'));
      if (host == "localhost" || host.compare(0, 4, "127.") == 0 || host == "[::1]") host = localNetworkAddress();
      return httpAnswer("200 OK", "text/html; charset=utf-8", plusCartLoaderPage(roms, message, host));
    }
    if (request.browserPath.compare(0, 5, "/rom/") == 0) {
      if (const ServedRom* rom = findRom(roms, request.browserPath.substr(4))) {
        if (logLine) *logLine = F("PlusCart: {0} downloaded from a browser.", {rom->name});
        return httpAnswer("200 OK", "application/octet-stream", readFile(rom->path),
                          "Content-Disposition: attachment; filename=\"" + rom->name + "\"\r\n");
      }
    }
    return httpAnswer("404 Not Found", "text/plain; charset=utf-8", "Not found\n");
  }
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
    for (auto connection = connections_.begin(); connection != connections_.end();) {  // the finished ones
      if (connection->finished->load()) {
        connection->thread.join();
        connection = connections_.erase(connection);
      } else {
        ++connection;
      }
    }
    SocketHandle client = accept(listener, nullptr, nullptr);
    if (client == kInvalidSocket) continue;
    auto finished = std::make_shared<std::atomic<bool>>(false);
    connections_.push_back({std::thread([this, client, finished] {
                              handleConnection((long long)client);
                              closeSocket(client);
                              *finished = true;
                            }),
                            finished});
  }
  for (auto& connection : connections_) connection.thread.join();  // each sees stopping_ within 200 ms
  connections_.clear();
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
