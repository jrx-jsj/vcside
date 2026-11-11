// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/model_list.h"

#include <algorithm>
#include <chrono>
#include <set>

namespace vcs::assistant {

namespace {
std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return text;
}
bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
}  // namespace

std::vector<std::string> normaliseModelIds(std::vector<std::string> ids, const Provider& provider) {
  bool google = provider.baseUrl.find("generativelanguage.googleapis.com") != std::string::npos;
  std::set<std::string> unique;
  for (auto id : ids) {
    if (google && id.rfind("models/", 0) == 0) id = id.substr(7);
    std::string lower = lowered(id);
    // not for chatting: embeddings, speech, images, video, moderation, ranking
    bool other = contains(lower, "embed") || contains(lower, "whisper") || contains(lower, "tts") || contains(lower, "dall-e") || contains(lower, "moderation") || contains(lower, "imagen") ||
                 contains(lower, "veo-") || contains(lower, "aqa") || contains(lower, "transcribe") || contains(lower, "rerank") || contains(lower, "-image") || contains(lower, "realtime");
    if (!id.empty() && !other) unique.insert(id);
  }
  return std::vector<std::string>(unique.begin(), unique.end());
}

std::string describeModelListError(const std::string& error) {
  if (error.find("401") != std::string::npos || error.find("403") != std::string::npos || lowered(error).find("api key") != std::string::npos || lowered(error).find("unauthor") != std::string::npos)
    return "The service refused the key: check it, or save it again.";
  if (error.find("404") != std::string::npos) return "The service has no list of models at this address; type the name of the model.";
  return error;
}

bool ModelLists::hasKey(const Provider& provider) {
  double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  Presence& presence = keyPresence_[provider.name + "|" + provider.keyEnv];
  if (now - presence.at > 3.0) presence.at = now, presence.present = !keyOf(provider).empty();
  return presence.present;
}

std::string ModelLists::keyFor(const Provider& provider) { return provider.baseUrl + "|" + provider.dialect + "|" + (hasKey(provider) ? "key" : "nokey"); }

bool ModelLists::needsKey(const Provider& provider) { return !provider.builtin && !provider.isLocal() && !hasKey(provider); }

void ModelLists::request(const Provider& provider, bool force) {
  if (provider.builtin || provider.baseUrl.empty() || needsKey(provider)) return;
  std::string key = provider.name + "|" + provider.baseUrl;
  if (pending_.count(key)) return;
  Entry& entry = entries_[key];
  if (!force && entry.fetched && entry.forKey == keyFor(provider)) return;
  entry.fetching = true, entry.error.clear(), entry.tries = 0;
  Provider copy = provider;
  Pending pending;
  pending.provider = provider;
  pending.future = std::async(std::launch::async, [copy]() {
    std::string error;
    auto ids = listModels(copy, &error);
    return std::make_pair(ids, error);
  });
  pending_[key] = std::move(pending);
}

void ModelLists::poll() {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++it;
      continue;
    }
    auto result = it->second.future.get();
    Entry& entry = entries_[it->first];
    std::string lowerError = lowered(result.second);
    bool lost = !result.second.empty() && (lowerError.find("timed out") != std::string::npos || lowerError.find("connection") != std::string::npos || lowerError.find("network") != std::string::npos);
    if (lost && entry.tries < 2) {  // the first connection of a run may be slow or fail: once more, by itself
      entry.tries++;
      Provider provider = it->second.provider;
      std::string key = it->first;
      it = pending_.erase(it);
      Pending again;
      again.provider = provider;
      again.future = std::async(std::launch::async, [provider]() {
        std::string error;
        auto ids = listModels(provider, &error);
        return std::make_pair(ids, error);
      });
      pending_[key] = std::move(again);
      continue;
    }
    entry.tries = 0;
    entry.fetching = false, entry.fetched = true;
    entry.forKey = keyFor(it->second.provider);
    entry.ids = normaliseModelIds(result.first, it->second.provider);
    entry.error = result.second.empty() ? (entry.ids.empty() ? "The service lists no models." : "") : describeModelListError(result.second);
    it = pending_.erase(it);
  }
}

const ModelLists::Entry& ModelLists::of(const Provider& provider) const {
  static const Entry none;
  auto found = entries_.find(provider.name + "|" + provider.baseUrl);
  return found == entries_.end() ? none : found->second;
}

}  // namespace vcs::assistant
