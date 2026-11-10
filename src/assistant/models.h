// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// The models VCSIDE can offer to download, where they are kept, and the download itself.
// Nothing is downloaded without the user's confirmation, and every file is checked against the
// SHA-256 pinned in data/assistant/models.json before it is used.
#pragma once
#include <cstdint>
#include <future>
#include <string>
#include <vector>

#include "child_process.h"

namespace vcs::assistant {

struct CatalogModel {
  std::string id, name, sizeClass, file, url, sha256, license, note;
  int64_t bytes = 0;
  int memoryGb = 0;  // memory the model needs to run comfortably
};

// data/assistant/models.json. Empty if the file is missing or malformed.
std::vector<CatalogModel> loadCatalog(const std::string& jsonPath);
// The folder the models live in (inside the user's settings folder).
std::string modelsDir();
std::string modelPath(const CatalogModel& model);          // where the file goes
bool isInstalled(const CatalogModel& model);               // present, complete and verified earlier
// The model that fits a machine with this much memory (GB): the largest that needs at most 60% of it.
const CatalogModel* recommendedFor(const std::vector<CatalogModel>& catalog, int machineMemoryGb);
// Physical memory in GB (0 when unknown).
int machineMemoryGb();
std::string formatBytes(int64_t bytes);  // "4.7 GB"

// One download at a time, with curl (macOS, Windows 10+ and nearly every Linux have it).
class Download {
 public:
  enum class State { Idle, Running, Verifying, Done, Failed };
  ~Download() {
    cancel();
    if (verification_.valid()) verification_.wait();
  }
  bool available() const;                      // curl found
  void start(const CatalogModel& model);       // resumes a partial file
  void cancel();                               // keeps the partial file so it can resume
  void poll();                                 // per frame
  State state() const { return state_; }
  double progress() const;                     // 0..1
  const std::string& error() const { return error_; }
  const CatalogModel& model() const { return model_; }

 private:
  Job job_;
  State state_ = State::Idle;
  CatalogModel model_;
  std::string error_;
  std::string partial_;
  std::future<std::string> verification_;
};

}  // namespace vcs::assistant
