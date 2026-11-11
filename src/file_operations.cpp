// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "file_operations.h"

#include <filesystem>
#include <fstream>

namespace vcs::fileops {

namespace fs = std::filesystem;

std::string nameProblem(const std::string& name) {
  if (name.empty()) return "The name is empty.";
  if (name == "." || name == "..") return "That is not a name.";
  if (name.front() == ' ' || name.back() == ' ' || name.back() == '.') return "A name cannot start or end with a space, or end with a dot.";
  for (char c : name) {
    if (c == '/' || c == '\\') return "A name cannot have / or \\ in it.";
    if (std::string("<>:\"|?*").find(c) != std::string::npos || (unsigned char)c < 0x20) return std::string("A name cannot have the character ") + (c < 0x20 ? "(control)" : std::string(1, c)) + ".";
  }
  return "";
}

namespace {
bool place(const std::string& folder, const std::string& name, fs::path* target, std::string* error) {
  std::string problem = nameProblem(name);
  if (!problem.empty()) {
    if (error) *error = problem;
    return false;
  }
  std::error_code code;
  if (!fs::is_directory(folder, code)) {
    if (error) *error = "The folder does not exist.";
    return false;
  }
  *target = fs::path(folder) / name;
  if (fs::exists(*target, code) || fs::is_symlink(*target, code)) {
    if (error) *error = "\"" + name + "\" already exists here.";
    return false;
  }
  return true;
}
}  // namespace

bool createFile(const std::string& folder, const std::string& name, std::string* created, std::string* error) {
  fs::path target;
  if (!place(folder, name, &target, error)) return false;
  std::ofstream out(target, std::ios::binary);
  if (!out) {
    if (error) *error = "The file could not be created.";
    return false;
  }
  if (created) *created = target.string();
  return true;
}

bool createFolder(const std::string& folder, const std::string& name, std::string* created, std::string* error) {
  fs::path target;
  if (!place(folder, name, &target, error)) return false;
  std::error_code code;
  if (!fs::create_directory(target, code) || code) {
    if (error) *error = "The folder could not be created.";
    return false;
  }
  if (created) *created = target.string();
  return true;
}

bool rename(const std::string& path, const std::string& newName, std::string* renamed, std::string* error) {
  fs::path from(path), target;
  if (fs::path(newName) == from.filename()) {  // the same name: nothing to do
    if (renamed) *renamed = path;
    return true;
  }
  if (!place(from.parent_path().string(), newName, &target, error)) return false;
  std::error_code code;
  fs::rename(from, target, code);
  if (code) {
    if (error) *error = "It could not be renamed: " + code.message() + ".";
    return false;
  }
  if (renamed) *renamed = target.string();
  return true;
}

bool duplicate(const std::string& path, std::string* copy, std::string* error) {
  fs::path from(path);
  std::error_code code;
  if (!fs::exists(from, code)) {
    if (error) *error = "It does not exist any more.";
    return false;
  }
  std::string stem = from.stem().string(), extension = from.extension().string();
  if (fs::is_directory(from, code)) stem = from.filename().string(), extension.clear();
  fs::path target;
  for (int number = 1; number < 1000; number++) {
    target = from.parent_path() / (stem + " copy" + (number > 1 ? " " + std::to_string(number) : "") + extension);
    if (!fs::exists(target, code) && !fs::is_symlink(target, code)) break;
  }
  fs::copy(from, target, fs::copy_options::recursive | fs::copy_options::copy_symlinks, code);
  if (code) {
    if (error) *error = "It could not be copied: " + code.message() + ".";
    return false;
  }
  if (copy) *copy = target.string();
  return true;
}

bool remove(const std::string& path, std::string* error) {
  std::error_code code;
  if (!fs::exists(path, code) && !fs::is_symlink(path, code)) {
    if (error) *error = "It does not exist any more.";
    return false;
  }
  fs::remove_all(path, code);
  if (code) {
    if (error) *error = "It could not be deleted: " + code.message() + ".";
    return false;
  }
  return true;
}

}  // namespace vcs::fileops
