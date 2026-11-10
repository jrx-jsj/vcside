// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "assistant/first_pass.h"

#include <nlohmann/json.hpp>

#include "assistant/prompt.h"

namespace vcs::assistant {

using json = nlohmann::json;

std::string cleanTitle(const std::string& text, size_t maxBytes) {
  std::string line;
  for (char c : sanitizeUtf8(text)) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && (line.empty() || line.back() == ' ')) continue;
    line += c;
  }
  auto strip = [&](const char* marks) {
    while (!line.empty() && std::string(marks).find(line.back()) != std::string::npos) line.pop_back();
    while (!line.empty() && std::string(marks).find(line.front()) != std::string::npos) line.erase(0, 1);
  };
  strip("\"'`*# .:;,");
  if (line.size() > maxBytes) line = utf8Cut(line, maxBytes - 3) + "...";
  return line;
}

FirstPass parseFirstPass(const std::string& modelText, bool needTitle, bool needEnglish) {
  FirstPass result;
  std::string text = withoutThinking(modelText);
  size_t open = text.find('{');
  size_t close = text.rfind('}');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    result.error = "no JSON in the reply";
    return result;
  }
  json value = json::parse(text.substr(open, close - open + 1), nullptr, false);
  if (!value.is_object()) {
    result.error = "the reply is not JSON";
    return result;
  }
  auto field = [&](const char* key) { return value.contains(key) && value[key].is_string() ? value[key].get<std::string>() : std::string(); };
  result.title = cleanTitle(field("title"));
  result.english = field("english");
  result.language = field("language");
  for (char& c : result.language) c = (char)std::tolower((unsigned char)c);
  if (result.language.size() > 8) result.language.clear();
  result.ok = (!needTitle || !result.title.empty()) && (!needEnglish || !result.english.empty());
  if (!result.ok) result.error = "the reply lacks " + std::string(needTitle && result.title.empty() ? "a title" : "the English text");
  return result;
}

void FirstPassRequest::start(const Provider& provider, const std::string& maskedQuestion, bool needTitle, bool needEnglish) {
  needTitle_ = needTitle, needEnglish_ = needEnglish;
  std::string fields = "\"language\": \"<ISO 639-1 code of the language the user writes in>\"";
  if (needTitle) fields += ", \"title\": \"<a title of at most 5 words, in the user's language, that names the task>\"";
  if (needEnglish) fields += ", \"english\": \"<the request translated to English; unchanged if it already is English>\"";
  std::string system = "You prepare a user's request for an assistant. Tokens like [[1]] stand for code or names: copy them exactly as they are, in the same places. Reply with ONLY a JSON object, nothing else:\n{" + fields + "}";
  // a long request is not translated whole: the title comes from its start
  request_.start(provider, system, utf8Cut(maskedQuestion, 3000), needEnglish ? 1200 : 120, 30);
}

bool FirstPassRequest::poll(FirstPass* result) {
  std::string text, error;
  if (!request_.poll(&text, &error)) return false;
  *result = error.empty() ? parseFirstPass(text, needTitle_, needEnglish_) : FirstPass();
  if (!error.empty()) result->error = error;
  return true;
}

std::string languageName(const std::string& isoCode) {
  static const char* names[][2] = {{"es", "Spanish"}, {"fr", "French"}, {"de", "German"}, {"it", "Italian"}, {"pt", "Portuguese"}, {"ca", "Catalan"}, {"gl", "Galician"}, {"eu", "Basque"},
                                   {"nl", "Dutch"}, {"pl", "Polish"}, {"ru", "Russian"}, {"ja", "Japanese"}, {"zh", "Chinese"}, {"ko", "Korean"}, {"tr", "Turkish"}, {"sv", "Swedish"}};
  for (auto& item : names)
    if (isoCode == item[0]) return item[1];
  return isoCode;
}

std::string translationPrompt(const std::string& to, const std::string& from) {
  return "Translate the user's text from " + from + " to " + to + ". Tokens like [[1]] stand for code or names: copy each one exactly as it is, in the place where it belongs, and never drop or add one. "
         "Keep the line breaks and the Markdown (lists, bold, headings). Do not explain and do not add anything. Reply with only the translation.";
}

}  // namespace vcs::assistant
