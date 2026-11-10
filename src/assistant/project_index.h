// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// An index of the project's own files, so the assistant can answer about the whole project and
// not only about the open file (docs/design-assistant.md, "The project index"). Files are cut
// into chunks at labels and functions, words are split like identifiers (WSYNC, kernel_loop,
// playerX), and a question is matched against them with BM25, a keyword ranking that needs no
// model. A "map" lists each file with the names it defines. Nothing leaves the process.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace vcs::assistant {

struct IndexHit {
  std::string file;        // relative to the project, with '/'
  int firstLine = 0, lastLine = 0;  // 1-based
  std::string text;
  double score = 0;
};

class ProjectIndex {
 public:
  // Reads again only the files whose size or time changed, and forgets the ones that are gone.
  // Returns how many files were read. Another folder starts from scratch.
  int update(const std::string& projectDir);

  size_t files() const { return fileCount_; }
  size_t chunks() const { return chunks_.size(); }

  // The best `limit` chunks for a question; chunks of `skipFile` are left out (it is attached
  // whole). Nothing is returned for a question that matches no more than noise.
  std::vector<IndexHit> search(const std::string& query, size_t limit, const std::string& skipFile = "") const;

  // One line per file: its name and the names it defines, the files and names the question
  // mentions first, up to maxChars.
  std::string map(const std::string& query, size_t maxChars) const;

  // The words of a text as the index sees them (lower case; identifiers split at '_', digits and
  // case changes, and kept whole too). Exposed for the tests.
  static std::vector<std::string> words(const std::string& text);

 private:
  struct Chunk {
    std::string file;
    int firstLine = 0, lastLine = 0;
    std::string text, symbol;
    std::map<std::string, int> counts;  // word -> occurrences (name words weigh more)
    int length = 0;
  };
  struct FileInfo {
    long long size = 0, time = 0;
    std::vector<std::string> symbols;
  };
  std::string dir_;
  std::map<std::string, FileInfo> fileInfo_;
  std::vector<Chunk> chunks_;
  size_t fileCount_ = 0;
  std::map<std::string, int> documentFrequency_;
  double averageLength_ = 1;
  void rebuildStatistics();
};

}  // namespace vcs::assistant
