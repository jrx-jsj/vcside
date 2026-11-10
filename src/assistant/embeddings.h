// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Meaning-based search for the project index (docs/design-assistant.md, "The project index"): a
// small embedding model runs in a second copy of the bundled helper, each chunk of the project
// becomes a vector, and a question is matched with them by cosine similarity. Vectors are cached
// on disk by the hash of the text, so only changed chunks are embedded again.
#pragma once
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vcs::assistant {

// Unit length, so that a dot product is the cosine. A zero vector stays zero.
std::vector<float> normalised(std::vector<float> vector);
float dotProduct(const std::vector<float>& left, const std::vector<float>& right);

// The hash a chunk's vector is cached under: the model and the text (hex SHA-256).
std::string embeddingKey(const std::string& modelId, const std::string& text);

// POST <baseUrl>/embeddings with the texts (llama.cpp's server and any OpenAI-style one). The
// vectors come back normalised, in the order of the texts. False with *error set on a failure.
bool requestEmbeddings(const std::string& baseUrl, const std::vector<std::string>& texts, std::vector<std::vector<float>>* vectors,
                       std::string* error, const std::atomic<bool>& cancel, int idleSeconds = 60);

// The vectors of one model for one project: <project>/.vcsai/index/<model id>.vec. Binary
// ("VCSV1", the number of dimensions, then each key and its floats); a damaged file is ignored.
class EmbeddingCache {
 public:
  bool load(const std::string& path);
  bool save(const std::string& path) const;
  const std::vector<float>* find(const std::string& key) const;
  void put(const std::string& key, std::vector<float> vector) { vectors_[key] = std::move(vector); }
  // Keeps only the keys in use, so the file does not grow with every edit.
  void keepOnly(const std::map<std::string, bool>& inUse);
  size_t size() const { return vectors_.size(); }

 private:
  std::map<std::string, std::vector<float>> vectors_;
};

// Embeds texts on a worker thread, a few at a time, and hands the vectors to the UI thread.
class EmbeddingWorker {
 public:
  struct Job {
    std::string key, text;
  };
  struct Done {
    std::string key;
    std::vector<float> vector;
  };
  ~EmbeddingWorker() { cancel(); }
  void start(const std::string& baseUrl, const std::string& documentPrefix, std::vector<Job> jobs);
  void cancel();
  bool running() const { return running_; }
  size_t total() const { return total_; }
  size_t finished() const { return finished_; }
  const std::string& error() const { return error_; }  // valid once not running
  std::vector<Done> poll();                              // what was embedded since the last call

 private:
  std::thread thread_;
  std::mutex mutex_;
  std::deque<Done> done_;
  std::atomic<bool> running_{false}, cancel_{false};
  std::atomic<size_t> total_{0}, finished_{0};
  std::string error_;
};

}  // namespace vcs::assistant
