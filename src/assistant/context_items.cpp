// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/context_items.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "assistant/prompt.h"
#include "assistant/tools.h"

namespace vcs::assistant {

namespace fs = std::filesystem;

// ---- the set -----------------------------------------------------------------------------------

bool ContextSet::hasSelectionChip() const { return sources_.hasSelection && !sources_.fileRelative.empty() && !selectionRemoved_; }
bool ContextSet::hasFileChip() const { return !sources_.hasSelection && !sources_.fileRelative.empty() && !fileRemoved_; }
bool ContextSet::hasErrorsChip() const { return sources_.hasErrors && !errorsRemoved_; }

void ContextSet::sync(const ContextSources& sources) {
  sources_ = sources;
  std::string fileKey = sources.fileRelative;
  std::string selectionKey = sources.hasSelection ? sources.fileRelative + ":" + std::to_string(sources.selectionFirst) + "-" + std::to_string(sources.selectionLast) : "";
  std::string errorsKey = sources.hasErrors ? sources.errorsKey : "";
  if (fileKey != fileKey_) fileRemoved_ = false, fileKey_ = fileKey;
  if (selectionKey != selectionKey_) selectionRemoved_ = false, selectionKey_ = selectionKey;
  if (errorsKey != errorsKey_) errorsRemoved_ = false, errorsKey_ = errorsKey;
}

std::vector<ContextChip> ContextSet::chips(const std::string& errorsLabel) const {
  std::vector<ContextChip> list;
  if (hasSelectionChip()) {
    std::string lines = sources_.selectionFirst == sources_.selectionLast ? std::to_string(sources_.selectionFirst) : std::to_string(sources_.selectionFirst) + "-" + std::to_string(sources_.selectionLast);
    list.push_back({ContextChip::Selection, sources_.fileRelative + ":" + lines, sources_.fileRelative, true});
  } else if (hasFileChip()) {
    list.push_back({ContextChip::File, sources_.fileRelative, sources_.fileRelative, true});
  }
  if (hasErrorsChip()) list.push_back({ContextChip::Errors, errorsLabel, "", true});
  for (auto& path : added_) list.push_back({ContextChip::Path, path, path, false});
  return list;
}

void ContextSet::remove(size_t index) {
  auto list = chips("");
  if (index >= list.size()) return;
  const ContextChip& chip = list[index];
  switch (chip.kind) {
    case ContextChip::Selection: selectionRemoved_ = true; break;
    case ContextChip::File: fileRemoved_ = true; break;
    case ContextChip::Errors: errorsRemoved_ = true; break;
    case ContextChip::Path: added_.erase(std::remove(added_.begin(), added_.end(), chip.path), added_.end()); break;
  }
}

void ContextSet::addFiles(const std::vector<std::string>& relativePaths) {
  for (auto& path : relativePaths)
    if (std::find(added_.begin(), added_.end(), path) == added_.end()) added_.push_back(path);
}

// ---- the @ list ------------------------------------------------------------------------------

namespace {
bool hidden(const std::string& name) { return name == ".git" || name == ".vcsai" || name == ".DS_Store" || name == "build" || (name.size() > 1 && name[0] == '.'); }

bool binaryName(const std::string& name) {
  static const char* binary[] = {".bin", ".a26", ".elf", ".o", ".a", ".png", ".jpg", ".gif", ".bmp", ".zip", ".wav", ".ogg", ".rom", ".pro", ".sym", ".lst", ".ico", ".icns", ".ace"};
  std::string lowered = name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  for (auto* extension : binary)
    if (lowered.size() >= std::strlen(extension) && lowered.compare(lowered.size() - std::strlen(extension), std::strlen(extension), extension) == 0) return true;
  return false;
}

std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return text;
}

// lower is better: the name starts with it, the name contains it, the path contains it, its letters in order
int matchRank(const std::string& path, const std::string& filter) {
  if (filter.empty()) return 0;
  std::string full = lowered(path);
  std::string name = lowered(fs::path(path.back() == '/' ? path.substr(0, path.size() - 1) : path).filename().string());
  std::string wanted = lowered(filter);
  if (name.rfind(wanted, 0) == 0) return 1;
  if (name.find(wanted) != std::string::npos) return 2;
  if (full.find(wanted) != std::string::npos) return 3;
  size_t at = 0;
  for (char c : wanted) {
    at = full.find(c, at);
    if (at == std::string::npos) return -1;
    at++;
  }
  return 4;
}
}  // namespace

std::vector<std::string> listProjectEntries(const std::string& projectDir, const std::string& filter, size_t limit) {
  std::vector<std::string> all;
  if (projectDir.empty()) return all;
  std::error_code ec;
  fs::recursive_directory_iterator it(projectDir, fs::directory_options::skip_permission_denied, ec), end;
  for (; it != end && all.size() < 4000; it.increment(ec)) {
    if (ec) break;
    std::string name = it->path().filename().string();
    bool directory = it->is_directory(ec);
    if (hidden(name)) {
      if (directory) it.disable_recursion_pending();
      continue;
    }
    if (it.depth() >= 6 && directory) it.disable_recursion_pending();
    if (!directory && (isSecretFile(name) || binaryName(name))) continue;
    std::string relative = fs::relative(it->path(), projectDir, ec).generic_string();
    all.push_back(directory ? relative + "/" : relative);
  }
  std::vector<std::pair<int, std::string>> ranked;
  for (auto& path : all) {
    int rank = matchRank(path, filter);
    if (rank >= 0) ranked.push_back({rank, path});
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    int depthA = (int)std::count(a.second.begin(), a.second.end(), '/'), depthB = (int)std::count(b.second.begin(), b.second.end(), '/');
    return depthA != depthB ? depthA < depthB : a.second < b.second;  // shallow paths first
  });
  std::vector<std::string> result;
  for (size_t i = 0; i < ranked.size() && i < limit; i++) result.push_back(ranked[i].second);
  return result;
}

std::vector<std::string> filesInFolder(const std::string& projectDir, const std::string& folderRelative, size_t maxFiles, size_t* total) {
  std::vector<std::string> files;
  std::string error, absolute;
  if (!resolveInProject(projectDir, folderRelative.empty() ? "." : folderRelative, &absolute, &error)) {
    if (total) *total = 0;
    return files;
  }
  std::error_code ec;
  fs::recursive_directory_iterator it(absolute, fs::directory_options::skip_permission_denied, ec), end;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    std::string name = it->path().filename().string();
    bool directory = it->is_directory(ec);
    if (hidden(name)) {
      if (directory) it.disable_recursion_pending();
      continue;
    }
    if (it.depth() >= 3 && directory) it.disable_recursion_pending();
    if (directory || isSecretFile(name) || binaryName(name) || !it->is_regular_file(ec)) continue;
    files.push_back(fs::relative(it->path(), projectDir, ec).generic_string());
  }
  std::sort(files.begin(), files.end());
  if (total) *total = files.size();
  if (files.size() > maxFiles) files.resize(maxFiles);
  return files;
}

// ---- what is sent ----------------------------------------------------------------------------

GatheredContext gatherContext(const std::vector<ContextChip>& chips, const ContextText& text, const std::string& projectDir, size_t budgetChars) {
  GatheredContext result;
  size_t left = budgetChars;
  std::vector<std::string> names;
  auto take = [&](const std::string& source, const std::string& label, std::string* into) {
    std::string piece = utf8Cut(source, left);
    if (piece.size() < source.size()) result.notes.push_back(label);
    left -= piece.size();
    *into = sanitizeUtf8(piece);
  };
  for (auto& chip : chips) {
    switch (chip.kind) {
      case ContextChip::Selection:
        result.selectionLabel = chip.label;
        take(text.selection, chip.label, &result.selection);
        names.push_back(chip.label);
        break;
      case ContextChip::File:
        result.fileName = chip.path;
        take(text.fileText, chip.path, &result.fileText);
        names.push_back(chip.label);
        break;
      case ContextChip::Errors:
        take(text.errors, chip.label, &result.errors);
        names.push_back(chip.label);
        break;
      case ContextChip::Path: {
        std::string content, error;
        if (!readProjectFile(projectDir, chip.path, &content, &error)) {
          result.notes.push_back(chip.path + ": " + error);
          break;
        }
        if (left == 0) {
          result.notes.push_back(chip.path);
          break;
        }
        std::string piece;
        take(content, chip.path, &piece);
        result.extraFiles.push_back({chip.path, piece});
        names.push_back(chip.label);
        break;
      }
    }
  }
  for (size_t i = 0; i < names.size(); i++) result.summary += (i ? ", " : "") + names[i];
  return result;
}

}  // namespace vcs::assistant
