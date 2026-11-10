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
// A device the helper can run a model on, as it reports them (`llama-server --list-devices`).
struct Device {
  std::string id, name;           // "MTL0", "Apple M1 Pro"
  int64_t totalMiB = 0, freeMiB = 0;
  bool gpu = false;               // false for the CPU-side libraries (BLAS, Accelerate)
};
// What the machine offers to run a model.
struct Hardware {
  int ramGb = 0, cpuThreads = 0;
  std::vector<Device> devices;
  bool probed = false;            // the helper has been asked
  const Device* bestGpu() const;  // the GPU with the most free memory, or nullptr
};
// Reads the helper's device list. Lines that are not "<id>: <name> (<total> MiB, <free> MiB free)" are ignored.
std::vector<Device> parseDeviceList(const std::string& helperOutput);
// Physical memory (GB) and logical CPUs; devices stay empty until probeDevices.
Hardware basicHardware();
// Runs `<helper> --list-devices` (a second or two): call it off the UI thread.
std::vector<Device> probeDevices(const std::string& helperPath);

struct Recommendation {
  const CatalogModel* model = nullptr;
  bool onGpu = false;
  std::string device;             // the GPU it runs on ("Apple M1 Pro"); empty on the CPU
  double memoryGb = 0;            // the GPU's free memory, or the computer's memory
};
// The largest model that runs well here: entirely on the best GPU when one has room for it,
// otherwise on the CPU within 60% of the RAM and never above 8 GB (larger ones are too slow without a GPU).
Recommendation recommend(const std::vector<CatalogModel>& catalog, const Hardware& hardware);
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
