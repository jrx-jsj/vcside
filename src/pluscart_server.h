// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Serves the project's ROMs to a PlusCart on the local network.
//
// The PlusCart asks its "API host" (Setup > API Host in its menu; pluscart.firmaplus.de
// by default) for its menu and its ROMs over HTTP on port 80. Pointed at the computer
// that runs VCSIDE, its menu lists the project's ROMs, and choosing one downloads and
// runs it. The protocol, as the UCA firmware uses it (esp8266.c):
// - GET /api.php?p=<path>, without Range: the menu at <path> ("" is the top). The answer
//   is a status line (account flag, how many path levels to drop, a message) and one line
//   per entry: its type, a space, its size in 6 digits, a space and its name.
// - The same with "Range: bytes=<first>-<last>": that part of the file at <path> (up to
//   32 KB per request, several requests on one keep-alive connection).
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcs {

struct ServedRom {
  std::string name;  // as the PlusCart shows it (and asks for it): up to 32 characters
  std::string path;  // the file on disk, read again on every request
};

// One request the PlusCart sends: the path asked for (decoded) and the byte range, if any.
struct PlusStoreRequest {
  bool valid = false;
  std::string path;
  bool hasRange = false;
  long first = 0, last = 0;
};
PlusStoreRequest parsePlusStoreRequest(const std::string& header);
// %XX and '+' (for a space), as the firmware encodes names in the path.
std::string decodePlusStorePath(const std::string& encoded);
// The menu text: the status line with `message`, then the ROMs (with their sizes) at the
// top, or just ".." anywhere else.
std::string plusStoreMenu(const std::vector<ServedRom>& roms, const std::string& path, const std::string& message);
// The complete HTTP answer to a request.
std::string plusStoreResponse(const PlusStoreRequest& request, const std::vector<ServedRom>& roms, const std::string& message,
                              std::string* logLine);

class PlusCartServer {
 public:
  ~PlusCartServer();
  // port 0: any free port (for tests); the PlusCart always uses 80.
  bool start(int port, std::string* error);
  void stop();
  bool running() const { return running_; }
  int port() const { return port_; }
  void setRoms(const std::vector<ServedRom>& roms, const std::string& message);
  std::vector<std::string> takeLog();  // what was served since the last call

 private:
  void serve();
  void handleConnection(long long client);
  std::vector<ServedRom> roms();

  std::thread thread_;
  std::atomic<bool> running_{false}, stopping_{false};
  long long listener_ = -1;
  int port_ = 0;
  std::mutex mutex_;
  std::vector<ServedRom> roms_;
  std::string message_;
  std::vector<std::string> log_;
};

// This computer's address on the local network (the one a PlusCart should use), or "".
std::string localNetworkAddress();

}  // namespace vcs
