// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Where an API key lives (docs/design-assistant.md, "Privacy and secrets"): by default in the
// operating system's secret store. The user may choose instead a file of VCSIDE's settings
// that only their account can read (0600): no question from the system, less protection.
// Never in a project. Without either the key is kept in memory for the session.
#pragma once
#include <string>
#include <vector>

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

// The file store: where it is, and whether keys go there instead of the system's store.
void setSecretFile(const std::string& path);
void useSecretFile(bool on);
bool usingSecretFile();
// Moves the keys of these accounts to the store now chosen (from the other one).
void moveSecrets(const std::vector<std::string>& accounts, bool toFile);

bool loadSecret(const std::string& account, std::string* secret);
bool saveSecret(const std::string& account, const std::string& secret);
bool removeSecret(const std::string& account);

// "...abcd": what the settings window shows of a key.
std::string maskKey(const std::string& key);
// Replaces every occurrence of the key in a text (logs, the request inspector).
std::string redactKey(std::string text, const std::string& key);

}  // namespace vcs::assistant
