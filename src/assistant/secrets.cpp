// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/secrets.h"

#include <map>
#include <mutex>

namespace vcs::assistant {

namespace {
const SecretBackend* backend_ = nullptr;
std::mutex mutex_;
std::map<std::string, std::string>& memory() {
  static std::map<std::string, std::string> secrets;
  return secrets;
}
}  // namespace

void setSecretBackend(const SecretBackend* backend) { backend_ = backend; }
bool secretsPersistent() { return backend_ != nullptr; }
const char* secretStoreName() { return backend_ ? backend_->name : "this session only"; }

bool loadSecret(const std::string& account, std::string* secret) {
  if (account.empty()) return false;
  if (backend_) return backend_->load(account, secret);
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = memory().find(account);
  if (found == memory().end()) return false;
  *secret = found->second;
  return true;
}

bool saveSecret(const std::string& account, const std::string& secret) {
  if (account.empty() || secret.empty()) return false;
  if (backend_) return backend_->save(account, secret);
  std::lock_guard<std::mutex> lock(mutex_);
  memory()[account] = secret;
  return true;
}

bool removeSecret(const std::string& account) {
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
