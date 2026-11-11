// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// What the project explorer's context menu does to the files and folders of a project: create, rename, duplicate and delete. No
// interface here, so it can be tested: each function returns false and says why in `error` (English; the caller shows it).
#pragma once
#include <string>

namespace vcs::fileops {

// A name for one file or folder: not empty, no path separators, not "." or "..", none of the characters Windows forbids, no
// leading or trailing space. Returns "" if it is fine, else the reason.
std::string nameProblem(const std::string& name);

bool createFile(const std::string& folder, const std::string& name, std::string* created, std::string* error);
bool createFolder(const std::string& folder, const std::string& name, std::string* created, std::string* error);
// The same folder, another name. Fails if the target exists.
bool rename(const std::string& path, const std::string& newName, std::string* renamed, std::string* error);
// "main.asm" -> "main copy.asm", then "main copy 2.asm"...; a folder is copied with everything in it.
bool duplicate(const std::string& path, std::string* copy, std::string* error);
// A file, or a folder with everything in it. Gone for good: the caller has asked first.
bool remove(const std::string& path, std::string* error);

}  // namespace vcs::fileops
