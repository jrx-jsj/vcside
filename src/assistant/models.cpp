// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/models.h"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "assistant/sha256.h"
#include "paths.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace vcs::assistant {

std::vector<CatalogModel> loadCatalog(const std::string& jsonPath) {
  std::vector<CatalogModel> models;
  std::ifstream input(jsonPath);
  json root = json::parse(input, nullptr, false);
  if (!root.is_object() || !root.contains("models") || !root["models"].is_array()) return models;
  auto text = [](const json& item, const char* key) { return item.contains(key) && item[key].is_string() ? item[key].get<std::string>() : std::string(); };
  for (auto& item : root["models"]) {
    if (!item.is_object()) continue;
    CatalogModel model;
    model.id = text(item, "id");
    model.name = text(item, "name");
    model.sizeClass = text(item, "class");
    model.file = text(item, "file");
    model.url = text(item, "url");
    model.sha256 = text(item, "sha256");
    model.license = text(item, "license");
    model.note = text(item, "note");
    model.bytes = item.value("bytes", (int64_t)0);
    model.memoryGb = item.value("memory_gb", 0);
    // A model without a pinned hash, an https address or a plain file name is not offered.
    bool plainName = !model.file.empty() && model.file.find_first_of("/\\") == std::string::npos && model.file.find("..") == std::string::npos;
    if (!model.id.empty() && plainName && model.sha256.size() == 64 && model.url.rfind("https://", 0) == 0) models.push_back(model);
  }
  return models;
}

std::string modelsDir() { return (fs::path(configDir()) / "models").string(); }
std::string modelPath(const CatalogModel& model) { return (fs::path(modelsDir()) / model.file).string(); }

bool isInstalled(const CatalogModel& model) {
  std::error_code code;
  return fs::is_regular_file(modelPath(model), code) && (int64_t)fs::file_size(modelPath(model), code) == model.bytes;
}

const Device* Hardware::bestGpu() const {
  const Device* best = nullptr;
  for (auto& device : devices)
    if (device.gpu && (!best || device.freeMiB > best->freeMiB)) best = &device;
  return best;
}

std::vector<Device> parseDeviceList(const std::string& output) {
  std::vector<Device> devices;
  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    // "  MTL0: Apple M1 Pro (12124 MiB, 12123 MiB free)"
    size_t colon = line.find(": ");
    size_t open = line.rfind(" (");
    size_t close = line.rfind(" MiB free)");
    if (colon == std::string::npos || open == std::string::npos || close == std::string::npos || open < colon) continue;
    Device device;
    size_t start = line.find_first_not_of(' ');
    device.id = line.substr(start, colon - start);
    device.name = line.substr(colon + 2, open - colon - 2);
    long long total = 0, free = 0;
    if (std::sscanf(line.c_str() + open, " (%lld MiB, %lld", &total, &free) != 2) continue;
    device.totalMiB = total, device.freeMiB = free;
    if (device.id.find(' ') != std::string::npos) continue;  // a sentence, not a device
    std::string upperId;
    for (char c : device.id) upperId += (char)std::toupper((unsigned char)c);
    device.gpu = upperId.rfind("BLAS", 0) != 0 && upperId.rfind("CPU", 0) != 0 && total > 0;
    devices.push_back(device);
  }
  return devices;
}

Hardware basicHardware() {
  Hardware hardware;
  hardware.ramGb = SDL_GetSystemRAM() / 1024;  // SDL reports MiB
  hardware.cpuThreads = SDL_GetNumLogicalCPUCores();
  return hardware;
}

std::vector<Device> probeDevices(const std::string& helperPath) {
  if (helperPath.empty()) return {};
  Job job;
  if (!job.start({helperPath, "--list-devices"}, "")) return {};
  for (int waited = 0; job.running() && waited < 15000; waited += 20) SDL_Delay(20);
  if (job.running()) job.stopAndWait();
  return parseDeviceList(job.output());
}

Recommendation recommend(const std::vector<CatalogModel>& catalog, const Hardware& hardware) {
  Recommendation result;
  auto largest = [&](auto fits) {
    const CatalogModel* best = nullptr;
    for (auto& model : catalog)
      if (fits(model) && (!best || model.memoryGb > best->memoryGb)) best = &model;
    return best;
  };
  if (const Device* gpu = hardware.bestGpu()) {
    double freeGb = gpu->freeMiB / 1024.0;
    if (const CatalogModel* model = largest([&](const CatalogModel& m) { return m.memoryGb <= freeGb; })) {
      return {model, true, gpu->name, freeGb};
    }
  }
  const CatalogModel* model = largest([&](const CatalogModel& m) { return m.memoryGb * 10 <= hardware.ramGb * 6 && m.memoryGb <= 8; });
  if (model) {
    return {model, false, "", (double)hardware.ramGb};
  }
  return result;
}

std::string formatBytes(int64_t bytes) {
  char text[32];
  if (bytes >= (int64_t)1 << 30) std::snprintf(text, sizeof text, "%.1f GB", bytes / 1073741824.0);
  else std::snprintf(text, sizeof text, "%.0f MB", bytes / 1048576.0);
  return text;
}

bool Download::available() const { return !findInPath("curl").empty(); }

void Download::start(const CatalogModel& model) {
  cancel();
  model_ = model;
  error_.clear();
  std::error_code code;
  fs::create_directories(modelsDir(), code);
  partial_ = modelPath(model) + ".part";
  std::string curl = findInPath("curl");
  if (curl.empty()) {
    state_ = State::Failed;
    error_ = "curl was not found: download the file by hand into the models folder.";
    return;
  }
  // -C - resumes a partial file; --fail turns an HTTP error into a failure instead of a saved error page.
  if (!job_.start({curl, "-L", "--fail", "--silent", "--show-error", "--proto", "=https", "-C", "-", "-o", partial_, model.url}, "")) {
    state_ = State::Failed;
    error_ = "curl could not be started.";
    return;
  }
  state_ = State::Running;
}

void Download::cancel() {
  if (state_ == State::Running) job_.stopAndWait();
  state_ = State::Idle;
}

double Download::progress() const {
  if (model_.bytes <= 0) return 0;
  std::error_code code;
  std::string path = state_ == State::Done ? modelPath(model_) : partial_;
  auto size = fs::exists(path, code) ? fs::file_size(path, code) : 0;
  double value = (double)size / (double)model_.bytes;
  return value > 1 ? 1 : value;
}

void Download::poll() {
  if (state_ == State::Running && job_.finished()) {
    if (job_.exitCode() != 0) {
      std::string log = job_.output();
      error_ = "The download failed: " + (log.size() > 400 ? log.substr(log.size() - 400) : log);
      state_ = State::Failed;
      return;
    }
    state_ = State::Verifying;
    // Hashing several GB takes a few seconds: not on the UI thread.
    verification_ = std::async(std::launch::async, [path = partial_] { return sha256OfFile(path); });
  }
  if (state_ == State::Verifying && verification_.valid() && verification_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    std::string hash = verification_.get();
    std::error_code code;
    if (hash != model_.sha256) {
      fs::remove(partial_, code);  // a wrong file must not be resumed
      error_ = "The downloaded file does not match the expected checksum, so it was deleted.";
      state_ = State::Failed;
    } else {
      fs::rename(partial_, modelPath(model_), code);
      if (code) {
        error_ = "The model could not be moved into place: " + code.message();
        state_ = State::Failed;
      } else {
        state_ = State::Done;
      }
    }
  }
}

}  // namespace vcs::assistant
