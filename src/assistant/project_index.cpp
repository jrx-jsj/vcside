// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/project_index.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "assistant/prompt.h"
#include "assistant/tools.h"

namespace vcs::assistant {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxFiles = 400, kMaxFileBytes = 256 * 1024, kMaxTotalBytes = 3 * 1024 * 1024;
constexpr int kChunkLines = 50, kMinChunkLines = 6;
constexpr size_t kChunkChars = 2400;

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return text;
}

bool wanted(const fs::path& path) {
  static const char* binary[] = {".bin", ".a26", ".elf", ".o", ".a", ".png", ".jpg", ".gif", ".bmp", ".zip", ".wav", ".ogg", ".rom", ".pro", ".sym", ".lst", ".ico", ".icns"};
  std::string extension = lower(path.extension().string());
  for (auto* skip : binary)
    if (extension == skip) return false;
  return !isSecretFile(path.filename().string());
}

const std::set<std::string>& stopWords() {
  static const std::set<std::string> words = {
      "the", "and", "for", "that", "this", "with", "from", "how", "what", "why", "where", "which", "does", "can", "you", "are", "not", "use", "using",
      "make", "get", "set", "has", "have", "into", "when", "then", "than", "file", "code", "project", "please", "show", "tell", "explain", "los", "las", "que", "con",
      "por", "para", "una", "uno", "como", "donde", "cual", "esta", "este", "del", "pero", "mas", "puedo", "hacer", "archivo", "codigo", "proyecto", "mira", "dime"};
  return words;
}

// "main.asm" -> words of the name too: a question about "the kernel" finds kernel.asm
std::vector<std::string> nameWords(const std::string& path) { return ProjectIndex::words(path); }

bool symbolLine(const std::string& line, std::string* name) {
  if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == ';' || line[0] == '#' || line[0] == '}' || line[0] == '/' || line[0] == '*' || line[0] == '{') return false;
  size_t end = 0;
  while (end < line.size() && (std::isalnum((unsigned char)line[end]) || line[end] == '_' || line[end] == '.')) end++;
  if (end == 0) return false;
  *name = line.substr(0, end);
  // an assembler keyword or a bare directive in column 0 is not a name worth keeping
  static const std::set<std::string> directives = {"processor", "include", "org", "seg", "mac", "endm", "if", "endif", "else", "echo", "align", "rorg", "repeat", "repend", "end", "rem", "dim", "const", "set", "data"};
  return directives.count(lower(*name)) == 0;
}

}  // namespace

std::vector<std::string> ProjectIndex::words(const std::string& text) {
  std::vector<std::string> result;
  size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && !std::isalnum((unsigned char)text[pos]) && text[pos] != '_') pos++;
    size_t start = pos;
    while (pos < text.size() && (std::isalnum((unsigned char)text[pos]) || text[pos] == '_')) pos++;
    if (pos == start) continue;
    std::string token = text.substr(start, pos - start);
    std::string whole = lower(token);
    if (whole.size() >= 2) result.push_back(whole);
    // parts: split at '_', at lower-to-upper changes and between letters and digits
    std::string part;
    std::vector<std::string> parts;
    for (size_t i = 0; i < token.size(); i++) {
      char c = token[i];
      bool boundary = c == '_' || (!part.empty() && ((std::islower((unsigned char)part.back()) && std::isupper((unsigned char)c)) ||
                                                       (std::isalpha((unsigned char)part.back()) != 0) != (std::isalpha((unsigned char)c) != 0)));
      if (boundary && !part.empty()) parts.push_back(part), part.clear();
      if (c != '_') part += c;
    }
    if (!part.empty()) parts.push_back(part);
    if (parts.size() > 1)
      for (auto& piece : parts)
        if (piece.size() >= 3) result.push_back(lower(piece));
  }
  return result;
}

int ProjectIndex::update(const std::string& projectDir) {
  if (projectDir != dir_) {
    dir_ = projectDir;
    fileInfo_.clear(), chunks_.clear(), documentFrequency_.clear(), fileCount_ = 0;
  }
  if (dir_.empty()) return 0;
  std::error_code ec;
  std::map<std::string, FileInfo> found;
  size_t totalBytes = 0;
  fs::recursive_directory_iterator it(dir_, fs::directory_options::skip_permission_denied, ec), end;
  for (; it != end && found.size() < kMaxFiles; it.increment(ec)) {
    if (ec) break;
    std::string name = it->path().filename().string();
    if (name == ".git" || name == ".vcsai" || name == "build" || name == ".DS_Store" || (name.size() > 1 && name[0] == '.' && it->is_directory(ec))) {
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec) || !wanted(it->path())) continue;
    long long size = (long long)it->file_size(ec);
    if (size <= 0 || size > (long long)kMaxFileBytes || totalBytes + (size_t)size > kMaxTotalBytes) continue;
    totalBytes += (size_t)size;
    FileInfo info;
    info.size = size;
    info.time = (long long)it->last_write_time(ec).time_since_epoch().count();
    found[fs::relative(it->path(), dir_, ec).generic_string()] = info;
  }
  int reread = 0;
  bool changed = false;
  // gone or changed files leave the chunks
  std::set<std::string> drop;
  for (auto& known : fileInfo_) {
    auto now = found.find(known.first);
    if (now == found.end() || now->second.size != known.second.size || now->second.time != known.second.time) drop.insert(known.first);
  }
  if (!drop.empty()) {
    chunks_.erase(std::remove_if(chunks_.begin(), chunks_.end(), [&](const Chunk& chunk) { return drop.count(chunk.file) > 0; }), chunks_.end());
    for (auto& name : drop) fileInfo_.erase(name);
    changed = true;
  }
  for (auto& item : found) {
    if (fileInfo_.count(item.first)) continue;
    std::ifstream in(fs::path(dir_) / item.first, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    if (text.find('\0') < std::min<size_t>(text.size(), 4096)) continue;  // binary
    text = sanitizeUtf8(text);
    FileInfo info = item.second;
    std::vector<std::string> lines;
    std::istringstream lineStream(text);
    std::string line;
    while (std::getline(lineStream, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
    auto nameTokens = nameWords(item.first);
    size_t start = 0;
    while (start < lines.size()) {
      size_t stop = start + 1;
      size_t chars = lines[start].size();
      while (stop < lines.size() && (int)(stop - start) < kChunkLines && chars < kChunkChars) {
        std::string symbol;
        bool boundary = (int)(stop - start) >= kMinChunkLines && symbolLine(lines[stop], &symbol);
        boundary = boundary || ((int)(stop - start) >= 30 && lines[stop].find_first_not_of(" \t") == std::string::npos);
        if (boundary) break;
        chars += lines[stop].size() + 1;
        stop++;
      }
      Chunk chunk;
      chunk.file = item.first, chunk.firstLine = (int)start + 1, chunk.lastLine = (int)stop;
      for (size_t i = start; i < stop; i++) chunk.text += lines[i] + "\n";
      bool blank = chunk.text.find_first_not_of(" \t\n") == std::string::npos;
      if (!blank) {
        std::string symbol;
        for (size_t i = start; i < stop && chunk.symbol.empty(); i++)
          if (symbolLine(lines[i], &symbol)) chunk.symbol = symbol;
        for (size_t i = start; i < stop; i++)
          if (symbolLine(lines[i], &symbol)) info.symbols.push_back(symbol);
        for (auto& word : words(chunk.text)) chunk.counts[word]++;
        for (auto& word : words(chunk.symbol)) chunk.counts[word] += 3;  // what the chunk is called weighs more
        for (auto& word : nameTokens) chunk.counts[word] += 2;           // so does the file's name
        for (auto& entry : chunk.counts) chunk.length += entry.second;
        chunks_.push_back(std::move(chunk));
      }
      start = stop;
    }
    {  // in the order they are defined, each once
      std::set<std::string> seen;
      std::vector<std::string> ordered;
      for (auto& name : info.symbols)
        if (seen.insert(name).second) ordered.push_back(name);
      info.symbols = ordered;
    }
    fileInfo_[item.first] = info;
    reread++;
    changed = true;
  }
  fileCount_ = fileInfo_.size();
  if (changed) rebuildStatistics();
  return reread;
}

void ProjectIndex::rebuildStatistics() {
  documentFrequency_.clear();
  double total = 0;
  for (auto& chunk : chunks_) {
    for (auto& entry : chunk.counts) documentFrequency_[entry.first]++;
    total += chunk.length;
  }
  averageLength_ = chunks_.empty() ? 1 : std::max(1.0, total / (double)chunks_.size());
}

std::vector<IndexHit> ProjectIndex::search(const std::string& query, size_t limit, const std::string& skipFile) const {
  std::vector<IndexHit> hits;
  if (chunks_.empty()) return hits;
  std::set<std::string> queryWords;
  for (auto& word : words(query))
    if (word.size() >= 3 && !stopWords().count(word)) queryWords.insert(word);
  if (queryWords.empty()) return hits;
  const double k1 = 1.2, b = 0.75, n = (double)chunks_.size();
  struct Scored {
    size_t index;
    double score;
    size_t matched;
  };
  std::vector<Scored> scored;
  for (size_t i = 0; i < chunks_.size(); i++) {
    const Chunk& chunk = chunks_[i];
    if (!skipFile.empty() && chunk.file == skipFile) continue;
    double score = 0;
    size_t matched = 0;
    for (auto& word : queryWords) {
      auto found = chunk.counts.find(word);
      if (found == chunk.counts.end()) continue;
      double df = (double)documentFrequency_.at(word);
      double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
      double tf = found->second;
      score += idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * chunk.length / averageLength_));
      matched++;
    }
    // a question of several words must find at least two of them (one is noise), or the name of the chunk
    bool nameMatch = false;
    for (auto& word : words(chunk.symbol)) nameMatch = nameMatch || queryWords.count(word) > 0;
    if (score > 0 && (matched >= 2 || queryWords.size() == 1 || nameMatch)) scored.push_back({i, score, matched});
  }
  std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& c) { return a.score > c.score; });
  for (size_t i = 0; i < scored.size() && hits.size() < limit; i++) {
    if (scored[i].score < 1.5) break;
    const Chunk& chunk = chunks_[scored[i].index];
    hits.push_back({chunk.file, chunk.firstLine, chunk.lastLine, chunk.text, scored[i].score});
  }
  return hits;
}

std::string ProjectIndex::map(const std::string& query, size_t maxChars) const {
  std::set<std::string> queryWords;
  for (auto& word : words(query)) queryWords.insert(word);
  struct Line {
    int mentioned;
    std::string text;
  };
  std::vector<Line> lines;
  for (auto& file : fileInfo_) {
    int mentioned = 0;
    for (auto& word : words(file.first)) mentioned += queryWords.count(word) ? 2 : 0;
    std::string text = file.first;
    if (!file.second.symbols.empty()) {
      text += ":";
      size_t shown = 0;
      for (auto& symbol : file.second.symbols) {
        for (auto& word : words(symbol)) mentioned += queryWords.count(word) ? 1 : 0;
        if (shown++ < 8) text += " " + symbol;
      }
      if (file.second.symbols.size() > 8) text += " ... (" + std::to_string(file.second.symbols.size()) + ")";
    }
    lines.push_back({mentioned, text});
  }
  std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.mentioned > b.mentioned; });
  std::string out;
  for (auto& line : lines) {
    if (out.size() + line.text.size() + 1 > maxChars) {
      out += "...\n";
      break;
    }
    out += line.text + "\n";
  }
  return out;
}

}  // namespace vcs::assistant
