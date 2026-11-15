// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "find_in_files.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace fs = std::filesystem;

namespace vcs::fif {

static char lowerAscii(char character) { return (character >= 'A' && character <= 'Z') ? (char)(character + 32) : character; }
static std::string lowerText(std::string text) {
  for (auto& character : text) character = lowerAscii(character);
  return text;
}
// The editor's word characters: assembly labels have dots (.loop), and any UTF-8 byte counts.
static bool isWordChar(char character) {
  return std::isalnum((unsigned char)character) || character == '_' || character == '.' || (unsigned char)character >= 0x80;
}
static bool wholeWordAt(const std::string& line, size_t start, size_t end) {
  return (start == 0 || !isWordChar(line[start - 1])) && (end >= line.size() || !isWordChar(line[end]));
}

// ---------------------------------------------------------------------------
// Matcher

Matcher::Matcher(const Query& query) : query_(query) {
  if (query_.text.empty()) {
    error_.clear();
    return;
  }
  if (query_.regex) {
    try {
      auto flags = std::regex::ECMAScript;
      if (!query_.matchCase) flags |= std::regex::icase;
      regex_ = std::regex(query_.text, flags);
      valid_ = true;
    } catch (const std::regex_error& regexError) {
      error_ = regexError.what();
    }
  } else {
    lowerText_ = query_.matchCase ? query_.text : lowerText(query_.text);
    valid_ = true;
  }
}

std::vector<Span> Matcher::find(const std::string& line) const {
  std::vector<Span> spans;
  if (!valid_) return spans;
  if (query_.regex) {
    for (auto match = std::sregex_iterator(line.begin(), line.end(), regex_); match != std::sregex_iterator(); ++match) {
      size_t start = (size_t)match->position(0), end = start + (size_t)match->length(0);
      if (end == start) continue;  // an empty match (a* or ^) shows nothing
      if (query_.wholeWord && !wholeWordAt(line, start, end)) continue;
      spans.push_back({(int)start, (int)end});
    }
    return spans;
  }
  const std::string searched = query_.matchCase ? line : lowerText(line);
  for (size_t position = searched.find(lowerText_); position != std::string::npos;
       position = searched.find(lowerText_, position + 1)) {
    size_t end = position + lowerText_.size();
    if (query_.wholeWord && !wholeWordAt(searched, position, end)) continue;
    spans.push_back({(int)position, (int)end});
    position = end - 1;  // matches do not overlap
  }
  return spans;
}

std::string Matcher::replace(const std::string& line, const std::string& replacement, int* count) const {
  int replaced = 0;
  std::string result;
  size_t copiedTo = 0;
  if (valid_ && query_.regex) {
    for (auto match = std::sregex_iterator(line.begin(), line.end(), regex_); match != std::sregex_iterator(); ++match) {
      size_t start = (size_t)match->position(0), end = start + (size_t)match->length(0);
      if (end == start || (query_.wholeWord && !wholeWordAt(line, start, end))) continue;
      result += line.substr(copiedTo, start - copiedTo);
      result += match->format(replacement);
      copiedTo = end;
      replaced++;
    }
  } else {
    for (const Span& span : find(line)) {
      result += line.substr(copiedTo, span.start - copiedTo);
      result += replacement;
      copiedTo = span.end;
      replaced++;
    }
  }
  result += line.substr(copiedTo);
  if (count) *count += replaced;
  return result;
}

std::string replaceInText(const std::string& text, const Matcher& matcher, const std::string& replacement, int* count) {
  std::string result;
  result.reserve(text.size());
  size_t start = 0;
  while (start <= text.size()) {
    size_t newline = text.find('\n', start);
    size_t lineEnd = newline == std::string::npos ? text.size() : newline;
    size_t contentEnd = lineEnd > start && text[lineEnd - 1] == '\r' ? lineEnd - 1 : lineEnd;
    result += matcher.replace(text.substr(start, contentEnd - start), replacement, count);
    result += text.substr(contentEnd, lineEnd - contentEnd);  // the \r, if any
    if (newline == std::string::npos) break;
    result += '\n';
    start = newline + 1;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Globs

static std::string trimmed(const std::string& text) {
  size_t first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
  return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

// Splits at the commas that are not inside braces.
static std::vector<std::string> splitTopLevel(const std::string& text) {
  std::vector<std::string> parts;
  std::string part;
  int depth = 0;
  for (char character : text) {
    if (character == '{') depth++;
    if (character == '}' && depth > 0) depth--;
    if (character == ',' && depth == 0) {
      parts.push_back(part), part.clear();
    } else {
      part += character;
    }
  }
  parts.push_back(part);
  return parts;
}

// "*.{asm,h}" -> "*.asm", "*.h"
static std::vector<std::string> expandBraces(const std::string& pattern) {
  size_t open = pattern.find('{');
  if (open == std::string::npos) return {pattern};
  int depth = 0;
  size_t close = std::string::npos;
  for (size_t index = open; index < pattern.size(); index++) {
    if (pattern[index] == '{') depth++;
    else if (pattern[index] == '}' && --depth == 0) { close = index; break; }
  }
  if (close == std::string::npos) return {pattern};  // unbalanced: taken literally
  std::vector<std::string> expanded;
  std::string before = pattern.substr(0, open), after = pattern.substr(close + 1);
  for (const std::string& choice : splitTopLevel(pattern.substr(open + 1, close - open - 1)))
    for (const std::string& rest : expandBraces(before + choice + after)) expanded.push_back(rest);
  return expanded;
}

// * any run of characters but "/", ** any run at all ("**/" also none), ? one character but "/".
// Case does not matter (file systems on macOS and Windows ignore it too).
static bool wildcardMatches(const char* pattern, const char* text) {
  while (*pattern) {
    if (pattern[0] == '*' && pattern[1] == '*') {
      const char* rest = pattern + 2;
      if (*rest == '/') {
        if (wildcardMatches(rest + 1, text)) return true;  // no folder at all
      }
      for (const char* from = text; ; from++) {
        if (wildcardMatches(rest, from)) return true;
        if (!*from) return false;
      }
    }
    if (*pattern == '*') {
      for (const char* from = text; ; from++) {
        if (wildcardMatches(pattern + 1, from)) return true;
        if (!*from || *from == '/') return false;
      }
    }
    if (!*text) return false;
    if (*pattern == '?') {
      if (*text == '/') return false;
    } else if (lowerAscii(*pattern) != lowerAscii(*text)) {
      return false;
    }
    pattern++, text++;
  }
  return !*text;
}

bool globMatches(const std::string& patternIn, const std::string& relativePath) {
  std::string pattern = trimmed(patternIn);
  std::replace(pattern.begin(), pattern.end(), '\\', '/');
  while (pattern.rfind("./", 0) == 0) pattern.erase(0, 2);
  while (!pattern.empty() && pattern[0] == '/') pattern.erase(0, 1);
  while (!pattern.empty() && pattern.back() == '/') pattern.pop_back();  // "build/": the folder
  if (pattern.empty()) return false;
  std::string path = relativePath;
  std::replace(path.begin(), path.end(), '\\', '/');
  for (const std::string& choice : expandBraces(pattern)) {
    if (choice.find('/') == std::string::npos) {  // a name: the file's or one of its folders'
      size_t start = 0;
      while (true) {
        size_t slash = path.find('/', start);
        std::string component = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (wildcardMatches(choice.c_str(), component.c_str())) return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
      }
    } else {  // a path: the file's, or a folder above it
      if (wildcardMatches(choice.c_str(), path.c_str())) return true;
      for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
        if (wildcardMatches(choice.c_str(), path.substr(0, slash).c_str())) return true;
    }
  }
  return false;
}

bool globListMatches(const std::string& patterns, const std::string& relativePath) {
  for (const std::string& pattern : splitTopLevel(patterns))
    if (!trimmed(pattern).empty() && globMatches(pattern, relativePath)) return true;
  return false;
}

static bool hasPatterns(const std::string& patterns) {
  for (const std::string& pattern : splitTopLevel(patterns))
    if (!trimmed(pattern).empty()) return true;
  return false;
}

// ---------------------------------------------------------------------------
// Files

bool looksBinary(const std::string& data) { return std::memchr(data.data(), 0, std::min<size_t>(data.size(), 8192)) != nullptr; }

// Calls `visit` with each file's relative path, in the Project panel's order; stops when it
// returns false.
static bool walkFiles(const Scope& scope, const Query& query, const fs::path& folder, const std::string& relativeFolder,
                      const std::function<bool(const std::string&)>& visit) {
  std::vector<fs::directory_entry> entries;
  std::error_code errorCode;
  for (auto& entry : fs::directory_iterator(folder, errorCode)) entries.push_back(entry);
  std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& first, const fs::directory_entry& second) {
    std::error_code ignored;
    bool firstFolder = first.is_directory(ignored), secondFolder = second.is_directory(ignored);
    if (firstFolder != secondFolder) return firstFolder;
    return lowerText(first.path().filename().string()) < lowerText(second.path().filename().string());
  });
  bool excluding = hasPatterns(query.exclude), including = hasPatterns(query.include);
  for (auto& entry : entries) {
    std::string name = entry.path().filename().string();
    if (name.empty()) continue;
    std::string relative = relativeFolder.empty() ? name : relativeFolder + "/" + name;
    if (scope.skipPaths.count(relative)) continue;
    std::error_code entryError;
    if (entry.is_directory(entryError)) {
      if (name[0] == '.' || scope.skipFolderNames.count(name)) continue;  // .git, .vscode...; build output
      if (excluding && globListMatches(query.exclude, relative)) continue;
      if (!walkFiles(scope, query, entry.path(), relative, visit)) return false;
    } else if (entry.is_regular_file(entryError)) {
      if (scope.skipExtensions.count(lowerText(entry.path().extension().string()))) continue;
      if (excluding && globListMatches(query.exclude, relative)) continue;
      if (including && !globListMatches(query.include, relative)) continue;
      if (!visit(relative)) return false;
    }
  }
  return true;
}

std::vector<std::string> listFiles(const Scope& scope, const Query& query) {
  std::vector<std::string> found;
  walkFiles(scope, query, fs::path(scope.root), "", [&](const std::string& relative) {
    found.push_back(relative);
    return true;
  });
  return found;
}

bool searchText(const std::string& text, const Matcher& matcher, FileResult& result, int maxMatches) {
  size_t start = 0;
  int lineNumber = 0;
  while (start <= text.size()) {
    size_t newline = text.find('\n', start);
    size_t lineEnd = newline == std::string::npos ? text.size() : newline;
    if (lineEnd > start && text[lineEnd - 1] == '\r') lineEnd--;
    std::string line = text.substr(start, lineEnd - start);
    std::vector<Span> spans = matcher.find(line);
    if (!spans.empty()) {
      bool full = result.matches + (int)spans.size() >= maxMatches;
      if (result.matches + (int)spans.size() > maxMatches) spans.resize(std::max(0, maxMatches - result.matches));
      result.matches += (int)spans.size();
      if (!spans.empty()) result.lines.push_back({lineNumber, std::move(line), std::move(spans)});
      if (full) return true;
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
    lineNumber++;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Search

void Search::start(const Query& query, const Scope& scope, int maxMatches) {
  cancel();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    found_.clear();
  }
  truncated_ = false;
  filesSearched_ = 0;
  running_ = true;
  thread_ = std::thread(&Search::work, this, query, scope, maxMatches);
}

void Search::cancel() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  stop_ = false;
  running_ = false;
}

void Search::take(std::vector<FileResult>& into) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& result : found_) into.push_back(std::move(result));
  found_.clear();
}

void Search::work(Query query, Scope scope, int maxMatches) {
  Matcher matcher(query);
  if (matcher.valid()) {
    std::error_code errorCode;
    fs::path root = fs::weakly_canonical(fs::path(scope.root), errorCode);
    if (errorCode) root = fs::path(scope.root);
    int total = 0;
    walkFiles(scope, query, root, "", [&](const std::string& relative) {
      if (stop_) return false;
      fs::path path = (root / fs::path(relative)).make_preferred();
      FileResult result;
      result.path = path.string(), result.relative = relative;
      std::string text;
      auto open = scope.openTexts.find(result.path);
      if (open != scope.openTexts.end()) {
        text = open->second, result.unsaved = true;
      } else {
        std::error_code sizeError;
        auto size = fs::file_size(path, sizeError);
        if (sizeError || (long long)size > scope.maxFileBytes) return true;
        std::ifstream in(path, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        text = contents.str();
        if (looksBinary(text)) return true;
      }
      filesSearched_++;
      bool full = searchText(text, matcher, result, maxMatches - total);
      total += result.matches;
      if (result.matches) {
        std::lock_guard<std::mutex> lock(mutex_);
        found_.push_back(std::move(result));
      }
      if (full) {
        truncated_ = true;
        return false;
      }
      return true;
    });
  }
  running_ = false;
}

}  // namespace vcs::fif
