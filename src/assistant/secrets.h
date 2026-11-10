// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Where an API key lives (docs/design-assistant.md, "Privacy and secrets"): only in the
// operating system's secret store, never in a file of VCSIDE or of a project. Where there
// is no store yet the key is kept in memory for the session and asked again next time.
#pragma once
#include <string>

namespace vcs::assistant {

struct SecretBackend {
  const char* name;  // "macOS Keychain"
  bool (*load)(const std::string& account, std::string* secret);
  bool (*save)(const std::string& account, const std::string& secret);
  bool (*remove)(const std::string& account);
};
// A system store, registered once at start. Without one the functions below use memory.
void setSecretBackend(const SecretBackend* backend);
bool secretsPersistent();           // a system store is registered
const char* secretStoreName();      // its name, or "this session only"

bool loadSecret(const std::string& account, std::string* secret);
bool saveSecret(const std::string& account, const std::string& secret);
bool removeSecret(const std::string& account);

// "...abcd": what the settings window shows of a key.
std::string maskKey(const std::string& key);
// Replaces every occurrence of the key in a text (logs, the request inspector).
std::string redactKey(std::string text, const std::string& key);

}  // namespace vcs::assistant
