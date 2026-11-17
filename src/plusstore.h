// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Uploads the project's ROM to the user's PlusStore account, so the PlusCart lists it in
// its menu with its usual API host (no Setup > API Host change, unlike pluscart_server.h).
//
// The PlusStore (plusstore.firmaplus.de) is a Nextcloud server; each account has a
// "My ROMs" folder the cartridge shows. VCSIDE uses Nextcloud's own ways:
// - Login Flow v2 the first time: the browser opens the PlusStore's login page, the user
//   logs in and grants access there, and VCSIDE gets an app password for itself (never the
//   user's password). It is kept in VCSIDE's keys file (keys.json in its settings folder,
//   which only the user's account can read, as the assistant's API keys) for the next
//   uploads, until the user signs out (which revokes it on the server).
// - WebDAV: MKCOL for the folders that are missing (if the user wants them made), PUT for
//   the file, under /remote.php/dav/files/<login>/.
// Everything runs on a worker thread; the UI polls for log lines and the URL to open.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcs {

constexpr const char* kPlusStoreServer = "https://plusstore.firmaplus.de";
constexpr const char* kPlusStoreDefaultFolder = "My ROMs/VCSIDE";

struct PlusStoreSettings {
  std::string folder = kPlusStoreDefaultFolder;  // in the account, '/' between folders
  bool createFolders = true;                     // make the ones that are missing
};

struct PlusStoreLogin {
  std::string server, loginName, appPassword;
  bool valid() const { return !server.empty() && !loginName.empty() && !appPassword.empty(); }
};

// "My ROMs/ VCSIDE/" -> {"My ROMs", "VCSIDE"}; empty when a part is "." or ".." (or the
// path is empty): the folder must be inside the account, by name.
std::vector<std::string> plusStoreFolderParts(const std::string& folder);
// The WebDAV URL of a file (or folder, with an empty name) under the user's files.
std::string plusStoreDavUrl(const PlusStoreLogin& login, const std::vector<std::string>& folders, const std::string& name);
// Percent-encoding for one URL path segment (RFC 3986 unreserved characters kept).
std::string encodePathSegment(const std::string& text);
std::string base64(const std::string& bytes);

// The answers of Login Flow v2, parsed; false when the JSON is not what Nextcloud sends.
struct LoginFlowStart {
  std::string loginUrl, pollEndpoint, token;
};
bool parseLoginFlowStart(const std::string& body, LoginFlowStart* start);
bool parseLoginFlowResult(const std::string& body, PlusStoreLogin* login);
// The login as kept in the secret store (JSON), and back.
std::string loginToSecret(const PlusStoreLogin& login);
PlusStoreLogin loginFromSecret(const std::string& secret);

class PlusStoreUploader {
 public:
  ~PlusStoreUploader();
  bool busy() const { return busy_; }
  bool available() const;  // HTTPS can be used on this system
  // Uploads the file as `name` into the settings' folder, logging in first if needed.
  void upload(const std::string& path, const std::string& name, const PlusStoreSettings& settings);
  void signIn();   // only the authorization
  void signOut();  // revokes the app password on the server and forgets it
  void cancel() { cancel_ = true; }
  // The account VCSIDE is authorized for ("" if none), from the secret store.
  std::string account();
  std::vector<std::string> takeLog();
  std::string takeUrlToOpen();  // the login page, once, for the UI thread to open

 private:
  void start(void (PlusStoreUploader::*job)());
  void uploadJob();
  void signInJob();
  void signOutJob();
  bool authorize(PlusStoreLogin* login);  // Login Flow v2, saved in the secret store
  bool makeFolders(const PlusStoreLogin& login, std::string* error);
  void log(const std::string& line);

  std::thread thread_;
  std::atomic<bool> busy_{false}, cancel_{false};
  std::mutex mutex_;
  std::vector<std::string> log_;
  std::string urlToOpen_;
  std::string path_, name_;
  PlusStoreSettings settings_;
  std::string cachedAccount_;
  bool accountKnown_ = false;
};

}  // namespace vcs
