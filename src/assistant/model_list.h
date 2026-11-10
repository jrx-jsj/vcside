// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The models a service offers, asked of the service itself (GET <address>/models) and kept per provider, so the model box of the
// settings and of the panel can be a list that is true today. Fetched on a worker thread; the UI polls every frame.
#pragma once
#include <future>
#include <map>
#include <string>
#include <vector>

#include "assistant/provider.h"

namespace vcs::assistant {

// What the service answered, ready to show: "models/gemini-2.5-flash" becomes "gemini-2.5-flash", entries that are not for
// chatting (embeddings, speech, images, moderation) are left out, each name once, sorted.
std::vector<std::string> normaliseModelIds(std::vector<std::string> ids, const Provider& provider);
// A failure in words the user can act on ("The service refused the key").
std::string describeModelListError(const std::string& error);

class ModelLists {
 public:
  struct Entry {
    std::vector<std::string> ids;
    std::string error;
    bool fetching = false, fetched = false;
    std::string forKey;  // what the list was asked with: address, protocol and whether there was a key
  };
  // Starts a fetch if there is none for this provider's address and key (or force). Nothing is asked of a remote service without a key.
  void request(const Provider& provider, bool force = false);
  void poll();  // every frame: takes the answers that arrived
  const Entry& of(const Provider& provider) const;
  bool needsKey(const Provider& provider) const;  // a remote service and no key saved
  static std::string keyFor(const Provider& provider);

 private:
  struct Pending {
    std::future<std::pair<std::vector<std::string>, std::string>> future;
    Provider provider;
  };
  std::map<std::string, Entry> entries_;
  std::map<std::string, Pending> pending_;
};

}  // namespace vcs::assistant
