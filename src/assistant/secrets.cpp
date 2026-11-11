// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/secrets.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vcs::assistant {

namespace {
const SecretBackend* backend_ = nullptr;
std::mutex mutex_;
std::string filePath_;
bool useFile_ = false;
std::map<std::string, std::string>& memory() {
  static std::map<std::string, std::string> secrets;
  return secrets;
}

// The file store: {"account": "key", ...}, readable and writable by the user's account only.
std::map<std::string, std::string> readFile() {
  std::map<std::string, std::string> keys;
  std::ifstream in(filePath_, std::ios::binary);
  if (!in) return keys;
  std::stringstream text;
  text << in.rdbuf();
  nlohmann::json value = nlohmann::json::parse(text.str(), nullptr, false);
  if (value.is_object())
    for (auto& [account, key] : value.items())
      if (key.is_string()) keys[account] = key.get<std::string>();
  return keys;
}

bool writeFile(const std::map<std::string, std::string>& keys) {
  namespace fs = std::filesystem;
  std::error_code error;
  fs::create_directories(fs::path(filePath_).parent_path(), error);
  std::string text = nlohmann::json(keys).dump(2) + "\n";
  std::string temporary = filePath_ + ".new";
#ifndef _WIN32
  // created with 0600 from the start, so the key is never readable by others, not even for a moment
  ::unlink(temporary.c_str());
  int handle = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (handle < 0) return false;
  bool written = ::write(handle, text.data(), text.size()) == (ssize_t)text.size();
  ::fchmod(handle, 0600);
  ::close(handle);
  if (!written) return ::unlink(temporary.c_str()), false;
#else
  // in the user's own profile folder, which only their account can read by default
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!(out << text)) return false;
  }
#endif
  fs::rename(temporary, filePath_, error);
  return !error;
}

bool fileActive() { return useFile_ && !filePath_.empty(); }
}  // namespace

void setSecretBackend(const SecretBackend* backend) { backend_ = backend; }
void setSecretFile(const std::string& path) { filePath_ = path; }
void useSecretFile(bool on) { useFile_ = on; }
bool usingSecretFile() { return fileActive(); }
bool secretsPersistent() { return fileActive() || backend_ != nullptr; }
const char* secretStoreName() { return fileActive() ? "a file only you can read" : backend_ ? backend_->name : "this session only"; }

void moveSecrets(const std::vector<std::string>& accounts, bool toFile) {
  if (filePath_.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  auto keys = readFile();
  bool changed = false;
  for (auto& account : accounts) {
    if (account.empty()) continue;
    std::string key;
    if (toFile) {
      bool found = false;
      if (backend_) found = backend_->load(account, &key);
      else if (auto inMemory = memory().find(account); inMemory != memory().end()) found = true, key = inMemory->second;
      if (!found || key.empty()) continue;
      keys[account] = key, changed = true;
      if (backend_) backend_->remove(account);
      else memory().erase(account);
    } else {
      auto found = keys.find(account);
      if (found == keys.end()) continue;
      bool saved = backend_ ? backend_->save(account, found->second) : (memory()[account] = found->second, true);
      if (saved) keys.erase(found), changed = true;
    }
  }
  if (changed) writeFile(keys);
}

bool loadSecret(const std::string& account, std::string* secret) {
  if (account.empty()) return false;
  if (fileActive()) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto keys = readFile();
    auto found = keys.find(account);
    if (found == keys.end()) return false;
    *secret = found->second;
    return true;
  }
  if (backend_) return backend_->load(account, secret);
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = memory().find(account);
  if (found == memory().end()) return false;
  *secret = found->second;
  return true;
}

bool saveSecret(const std::string& account, const std::string& secret) {
  if (account.empty() || secret.empty()) return false;
  if (fileActive()) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto keys = readFile();
    keys[account] = secret;
    return writeFile(keys);
  }
  if (backend_) return backend_->save(account, secret);
  std::lock_guard<std::mutex> lock(mutex_);
  memory()[account] = secret;
  return true;
}

bool removeSecret(const std::string& account) {
  if (fileActive()) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto keys = readFile();
    if (!keys.erase(account)) return false;
    return writeFile(keys);
  }
  if (backend_) return backend_->remove(account);
  std::lock_guard<std::mutex> lock(mutex_);
  return memory().erase(account) > 0;
}

std::string maskKey(const std::string& key) {
  if (key.size() <= 8) return "...";
  return "..." + key.substr(key.size() - 4);
}

std::string redactKey(std::string text, const std::string& key) {
  if (key.size() < 8) return text;
  for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at))
    text.replace(at, key.size(), "[key]"), at += 5;
  return text;
}

}  // namespace vcs::assistant
