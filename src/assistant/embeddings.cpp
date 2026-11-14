// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/embeddings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "assistant/http.h"
#include "assistant/prompt.h"
#include "assistant/provider.h"
#include "assistant/sha256.h"

namespace vcs::assistant {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::vector<float> normalised(std::vector<float> vector) {
  double sum = 0;
  for (float value : vector) sum += (double)value * value;
  if (sum <= 0) return vector;
  float scale = (float)(1.0 / std::sqrt(sum));
  for (float& value : vector) value *= scale;
  return vector;
}

float dotProduct(const std::vector<float>& left, const std::vector<float>& right) {
  if (left.size() != right.size()) return 0;
  float sum = 0;
  for (size_t i = 0; i < left.size(); i++) sum += left[i] * right[i];
  return sum;
}

std::string embeddingKey(const std::string& modelId, const std::string& text) {
  Sha256 hash;
  std::string material = modelId + "\n" + text;
  hash.update(material.data(), material.size());
  return hash.hexDigest();
}

bool requestEmbeddings(const std::string& baseUrl, const std::vector<std::string>& texts, std::vector<std::vector<float>>* vectors,
                       std::string* error, const std::atomic<bool>& cancel, int idleSeconds) {
  vectors->clear();
  if (texts.empty()) return true;
  json body = {{"input", json::array()}, {"encoding_format", "float"}};
  for (auto& text : texts) body["input"].push_back(sanitizeUtf8(text));
  HttpRequest request;
  request.method = "POST";
  request.url = joinUrl(baseUrl, "embeddings");
  request.headers = {{"Content-Type", "application/json"}};
  request.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
  request.connectSeconds = 3;
  request.idleSeconds = idleSeconds;
  std::string answer;
  HttpResult result = httpRequest(request, [&](const char* bytes, size_t size) { return answer.append(bytes, size), answer.size() < (64u << 20); }, cancel);
  if (result.status / 100 != 2 || !result.error.empty()) {
    if (error) {
      if (!result.error.empty()) {
        *error = result.error;
      } else {  // the server's own message when it sends one ({"error":{"message":...}}), not the whole JSON
        json body = json::parse(result.errorBody, nullptr, false);
        std::string message;
        if (body.is_object() && body.contains("error") && body["error"].is_object() && body["error"].contains("message") && body["error"]["message"].is_string())
          message = body["error"]["message"].get<std::string>();
        *error = "HTTP " + std::to_string(result.status) + ": " + (message.empty() ? result.errorBody.substr(0, 200) : utf8Cut(message, 200));
      }
    }
    return false;
  }
  json value = json::parse(answer, nullptr, false);
  if (!value.is_object() || !value.contains("data") || !value["data"].is_array() || value["data"].size() != texts.size()) {
    if (error) *error = "the embedding server answered something unexpected";
    return false;
  }
  vectors->resize(texts.size());
  for (auto& item : value["data"]) {
    size_t index = item.value("index", (size_t)0);
    if (index >= texts.size() || !item.contains("embedding") || !item["embedding"].is_array()) {
      if (error) *error = "the embedding server answered something unexpected";
      return false;
    }
    std::vector<float> vector;
    for (auto& number : item["embedding"]) vector.push_back(number.get<float>());
    (*vectors)[index] = normalised(std::move(vector));
  }
  return true;
}

// ---- the cache -----------------------------------------------------------------------------

bool EmbeddingCache::load(const std::string& path) {
  vectors_.clear();
  std::ifstream in(path, std::ios::binary);
  char magic[5] = {0};
  uint32_t dimensions = 0, count = 0;
  if (!in.read(magic, 5) || std::memcmp(magic, "VCSV1", 5) != 0) return false;
  if (!in.read((char*)&dimensions, 4) || !in.read((char*)&count, 4) || dimensions == 0 || dimensions > 8192 || count > 1000000) return false;
  for (uint32_t i = 0; i < count; i++) {
    char key[64];
    std::vector<float> vector(dimensions);
    if (!in.read(key, 64) || !in.read((char*)vector.data(), (std::streamsize)dimensions * 4)) {
      vectors_.clear();  // damaged: start again
      return false;
    }
    vectors_[std::string(key, 64)] = std::move(vector);
  }
  return true;
}

bool EmbeddingCache::save(const std::string& path) const {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary);
    uint32_t dimensions = vectors_.empty() ? 0 : (uint32_t)vectors_.begin()->second.size(), count = (uint32_t)vectors_.size();
    out.write("VCSV1", 5);
    out.write((const char*)&dimensions, 4);
    out.write((const char*)&count, 4);
    for (auto& item : vectors_) {
      out.write(item.first.data(), 64);
      out.write((const char*)item.second.data(), (std::streamsize)item.second.size() * 4);
    }
    if (!out) return false;
  }
  fs::rename(temporary, path, ec);
  return !ec;
}

const std::vector<float>* EmbeddingCache::find(const std::string& key) const {
  auto found = vectors_.find(key);
  return found == vectors_.end() ? nullptr : &found->second;
}

void EmbeddingCache::keepOnly(const std::map<std::string, bool>& inUse) {
  for (auto it = vectors_.begin(); it != vectors_.end();) it = inUse.count(it->first) ? std::next(it) : vectors_.erase(it);
}

// ---- the worker ----------------------------------------------------------------------------

void EmbeddingWorker::cancel() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
  running_ = false;
}

void EmbeddingWorker::start(const std::string& baseUrl, const std::string& documentPrefix, std::vector<Job> jobs) {
  cancel();
  cancel_ = false;
  error_.clear();
  total_ = jobs.size(), finished_ = 0;
  { std::lock_guard<std::mutex> lock(mutex_); done_.clear(); }
  running_ = true;
  thread_ = std::thread([this, baseUrl, documentPrefix, jobs = std::move(jobs)]() {
    const size_t batch = 8;
    for (size_t first = 0; first < jobs.size() && !cancel_; first += batch) {
      size_t last = std::min(jobs.size(), first + batch);
      std::vector<std::string> texts;
      for (size_t i = first; i < last; i++) texts.push_back(documentPrefix + utf8Cut(jobs[i].text, kEmbeddingTextBytes - std::min(documentPrefix.size(), kEmbeddingTextBytes / 2)));
      std::vector<std::vector<float>> vectors;
      std::string error;
      if (!requestEmbeddings(baseUrl, texts, &vectors, &error, cancel_, 120)) {
        if (!cancel_) error_ = error;
        break;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = first; i < last; i++) done_.push_back({jobs[i].key, std::move(vectors[i - first])});
      }
      finished_ = last;
    }
    running_ = false;
  });
}

std::vector<EmbeddingWorker::Done> EmbeddingWorker::poll() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Done> taken(std::make_move_iterator(done_.begin()), std::make_move_iterator(done_.end()));
  done_.clear();
  return taken;
}

}  // namespace vcs::assistant
