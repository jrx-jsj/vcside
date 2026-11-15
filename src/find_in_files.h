// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// Find in files: searches the project's text files (or the editors' unsaved text) for a
// word, a string or a regular expression, line by line, in a background thread, and
// replaces what it found. The panel that shows it is App::findInFilesPanel.
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace vcs::fif {

struct Query {
  std::string text;
  bool matchCase = false, wholeWord = false, regex = false;
  // Comma-separated globs ("*.asm, src/**"): only the files that match one of include (if
  // any), and none of exclude. A pattern without "/" matches the file name or any folder
  // in its path; with "/", the path from the project folder (or a folder above the file).
  std::string include, exclude;
};

// Byte offsets in a line: [start, end).
struct Span {
  int start = 0, end = 0;
};

// Finds a query in single lines and replaces it. A regular expression that does not
// compile leaves it invalid, with the compiler's message in error().
class Matcher {
 public:
  explicit Matcher(const Query& query);
  bool valid() const { return valid_; }
  const std::string& error() const { return error_; }
  std::vector<Span> find(const std::string& line) const;
  // The line with every match replaced; a regular expression's replacement can use $1, $&...
  std::string replace(const std::string& line, const std::string& replacement, int* count = nullptr) const;

 private:
  Query query_;
  std::string lowerText_;
  std::regex regex_;
  bool valid_ = false;
  std::string error_;
};

// Whether a relative path ("src/gfx/hero.asm", with "/") matches one glob, or a list of
// them separated by commas ("*.asm, *.h"); {a,b} picks one of several, ** spans folders.
bool globMatches(const std::string& pattern, const std::string& relativePath);
bool globListMatches(const std::string& patterns, const std::string& relativePath);

// Text with each line's matches replaced ("\n" or "\r\n" line ends are kept); the count of
// replacements in *count.
std::string replaceInText(const std::string& text, const Matcher& matcher, const std::string& replacement, int* count = nullptr);

// Where to search: a folder and what to leave out.
struct Scope {
  std::string root;                               // the project's folder
  std::set<std::string> skipPaths;                // relative paths ("/"), files or folders (build output, Git-ignored)
  std::set<std::string> skipFolderNames = {"build"};  // folders left out wherever they are
  std::set<std::string> skipExtensions;           // lower-case, with the dot (".vcsgfx")
  std::map<std::string, std::string> openTexts;   // canonical path -> the text an editor has (unsaved)
  long long maxFileBytes = 4 * 1024 * 1024;       // larger files are not searched
};

// The relative paths of the files a search looks at, in the Project panel's order
// (folders first, then by name). Dot folders and the scope's exclusions are left out.
std::vector<std::string> listFiles(const Scope& scope, const Query& query);

// Text with a NUL byte in its first 8 KB: a binary file, not searched.
bool looksBinary(const std::string& data);

struct ResultLine {
  int line = 0;  // 0-based
  std::string text;
  std::vector<Span> spans;
};

struct FileResult {
  std::string path;      // absolute (canonical)
  std::string relative;  // from the project folder, with "/"
  std::vector<ResultLine> lines;
  int matches = 0;
  bool unsaved = false;  // searched in an editor's text, not on disk
};

// Adds the matches of a text to a result; stops at maxMatches (true if it had to).
bool searchText(const std::string& text, const Matcher& matcher, FileResult& result, int maxMatches);

// A search running in its own thread; the panel takes what it found every frame.
class Search {
 public:
  Search() = default;
  ~Search() { cancel(); }
  Search(const Search&) = delete;
  Search& operator=(const Search&) = delete;
  // Starts again (an earlier search still running is stopped); at most maxMatches matches.
  void start(const Query& query, const Scope& scope, int maxMatches = 2000);
  void cancel();
  bool running() const { return running_; }
  // Moves the files found since the last call to the end of `into`.
  void take(std::vector<FileResult>& into);
  bool truncated() const { return truncated_; }   // stopped at maxMatches: there are more
  int filesSearched() const { return filesSearched_; }

 private:
  void work(Query query, Scope scope, int maxMatches);
  std::thread thread_;
  std::atomic<bool> stop_{false}, running_{false}, truncated_{false};
  std::atomic<int> filesSearched_{0};
  std::mutex mutex_;
  std::vector<FileResult> found_;
};

}  // namespace vcs::fif
