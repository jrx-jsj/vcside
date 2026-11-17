// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
#include "plusstore.h"

#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "assistant/http.h"
#include "assistant/secrets.h"
#include "i18n.h"

namespace vcs {

namespace {
// The secret store's account name for the PlusStore login (the assistant's are provider names).
constexpr const char* kSecretAccount = "PlusStore login";
constexpr int kLoginFlowSeconds = 20 * 60;  // as long as Nextcloud keeps a login flow open

std::string readWholeFile(const std::string& path, bool* ok) {
  std::ifstream file(path, std::ios::binary);
  *ok = (bool)file;
  std::stringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

struct Answer {
  int status = 0;
  std::string body, error;
};

Answer send(const std::string& method, const std::string& url, const PlusStoreLogin* login, const std::string& body,
            std::vector<std::pair<std::string, std::string>> headers, const std::atomic<bool>& cancel) {
  assistant::HttpRequest request;
  request.method = method;
  request.url = url;
  request.body = body;
  request.connectSeconds = 15;
  request.idleSeconds = 60;
  if (login) headers.push_back({"Authorization", "Basic " + base64(login->loginName + ":" + login->appPassword)});
  request.headers = headers;
  Answer answer;
  assistant::HttpResult result = assistant::httpRequest(request, [&answer](const char* bytes, size_t size) {
    answer.body.append(bytes, size);
    return true;
  }, cancel);
  answer.status = result.status;
  answer.error = result.error;
  if (answer.status / 100 != 2) answer.body = result.errorBody;
  return answer;
}

std::string httpProblem(const Answer& answer) {
  if (answer.status == 0) return answer.error.empty() ? std::string(T("no answer")) : answer.error;
  return "HTTP " + std::to_string(answer.status);
}
}  // namespace

std::vector<std::string> plusStoreFolderParts(const std::string& folder) {
  std::vector<std::string> parts;
  std::string part;
  auto finish = [&parts, &part]() {
    size_t first = part.find_first_not_of(' '), last = part.find_last_not_of(' ');
    std::string trimmed = first == std::string::npos ? "" : part.substr(first, last - first + 1);
    if (!trimmed.empty()) parts.push_back(trimmed);
    part.clear();
  };
  for (char character : folder) {
    if (character == '/' || character == '\\') finish();
    else part += character;
  }
  finish();
  for (auto& name : parts)
    if (name == "." || name == "..") return {};
  return parts;
}

std::string encodePathSegment(const std::string& text) {
  std::string encoded;
  char hex[4];
  for (unsigned char character : text) {
    if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
      encoded += (char)character;
    } else {
      std::snprintf(hex, sizeof hex, "%%%02X", character);
      encoded += hex;
    }
  }
  return encoded;
}

std::string plusStoreDavUrl(const PlusStoreLogin& login, const std::vector<std::string>& folders, const std::string& name) {
  std::string url = login.server + "/remote.php/dav/files/" + encodePathSegment(login.loginName) + "/";
  for (auto& folder : folders) url += encodePathSegment(folder) + "/";
  if (!name.empty()) url += encodePathSegment(name);
  return url;
}

std::string base64(const std::string& bytes) {
  static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  size_t index = 0;
  for (; index + 2 < bytes.size(); index += 3) {
    unsigned value = ((unsigned char)bytes[index] << 16) | ((unsigned char)bytes[index + 1] << 8) | (unsigned char)bytes[index + 2];
    encoded += {digits[value >> 18], digits[(value >> 12) & 63], digits[(value >> 6) & 63], digits[value & 63]};
  }
  if (index + 1 == bytes.size()) {
    unsigned value = (unsigned char)bytes[index] << 16;
    encoded += {digits[value >> 18], digits[(value >> 12) & 63], '=', '='};
  } else if (index + 2 == bytes.size()) {
    unsigned value = ((unsigned char)bytes[index] << 16) | ((unsigned char)bytes[index + 1] << 8);
    encoded += {digits[value >> 18], digits[(value >> 12) & 63], digits[(value >> 6) & 63], '='};
  }
  return encoded;
}

bool parseLoginFlowStart(const std::string& body, LoginFlowStart* start) {
  try {
    auto json = nlohmann::json::parse(body);
    start->loginUrl = json.at("login").get<std::string>();
    start->pollEndpoint = json.at("poll").at("endpoint").get<std::string>();
    start->token = json.at("poll").at("token").get<std::string>();
  } catch (...) {
    return false;
  }
  return start->loginUrl.rfind("https://", 0) == 0 && start->pollEndpoint.rfind("https://", 0) == 0 && !start->token.empty();
}

bool parseLoginFlowResult(const std::string& body, PlusStoreLogin* login) {
  try {
    auto json = nlohmann::json::parse(body);
    login->server = json.at("server").get<std::string>();
    login->loginName = json.at("loginName").get<std::string>();
    login->appPassword = json.at("appPassword").get<std::string>();
  } catch (...) {
    return false;
  }
  while (!login->server.empty() && login->server.back() == '/') login->server.pop_back();
  return login->valid() && login->server.rfind("https://", 0) == 0;
}

std::string loginToSecret(const PlusStoreLogin& login) {
  return nlohmann::json{{"server", login.server}, {"login", login.loginName}, {"app_password", login.appPassword}}.dump();
}

PlusStoreLogin loginFromSecret(const std::string& secret) {
  PlusStoreLogin login;
  try {
    auto json = nlohmann::json::parse(secret);
    login.server = json.value("server", "");
    login.loginName = json.value("login", "");
    login.appPassword = json.value("app_password", "");
  } catch (...) {
  }
  return login;
}

// ---- the worker ------------------------------------------------------------------------------

PlusStoreUploader::~PlusStoreUploader() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
}

bool PlusStoreUploader::available() const { return assistant::httpsAvailable(); }

void PlusStoreUploader::log(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_.push_back(line);
}

std::vector<std::string> PlusStoreUploader::takeLog() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> lines;
  lines.swap(log_);
  return lines;
}

std::string PlusStoreUploader::takeUrlToOpen() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string url;
  url.swap(urlToOpen_);
  return url;
}

std::string PlusStoreUploader::account() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!accountKnown_) {
    std::string secret;
    cachedAccount_ = assistant::loadFileSecret(kSecretAccount, &secret) ? loginFromSecret(secret).loginName : "";
    accountKnown_ = true;
  }
  return cachedAccount_;
}

void PlusStoreUploader::start(void (PlusStoreUploader::*job)()) {
  if (busy_) return;
  if (thread_.joinable()) thread_.join();
  cancel_ = false;
  busy_ = true;
  thread_ = std::thread([this, job] {
    (this->*job)();
    busy_ = false;
  });
}

void PlusStoreUploader::upload(const std::string& path, const std::string& name, const PlusStoreSettings& settings) {
  if (busy_) return;
  path_ = path, name_ = name, settings_ = settings;
  start(&PlusStoreUploader::uploadJob);
}

void PlusStoreUploader::signIn() { start(&PlusStoreUploader::signInJob); }
void PlusStoreUploader::signOut() { start(&PlusStoreUploader::signOutJob); }

bool PlusStoreUploader::authorize(PlusStoreLogin* login) {
  Answer started = send("POST", std::string(kPlusStoreServer) + "/index.php/login/v2", nullptr, "", {}, cancel_);
  LoginFlowStart flow;
  if (started.status != 200 || !parseLoginFlowStart(started.body, &flow)) {
    log(F("PlusStore: cannot start the authorization ({0}).", {httpProblem(started)}));
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    urlToOpen_ = flow.loginUrl;
  }
  log(F("PlusStore: log in and grant access in the browser page that opened ({0}). VCSIDE waits for it.", {flow.loginUrl}));
  std::string token = "token=" + encodePathSegment(flow.token);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kLoginFlowSeconds);
  while (!cancel_ && std::chrono::steady_clock::now() < deadline) {
    for (int tick = 0; tick < 20 && !cancel_; tick++) std::this_thread::sleep_for(std::chrono::milliseconds(100));  // every 2 s
    if (cancel_) break;
    Answer polled = send("POST", flow.pollEndpoint, nullptr, token, {{"Content-Type", "application/x-www-form-urlencoded"}}, cancel_);
    if (polled.status == 404) continue;  // not granted yet
    if (polled.status == 200 && parseLoginFlowResult(polled.body, login)) {
      if (!assistant::saveFileSecret(kSecretAccount, loginToSecret(*login)))
        log(T("PlusStore: the authorization could not be saved in VCSIDE's keys file: authorize again next time."));
      {
        std::lock_guard<std::mutex> lock(mutex_);
        cachedAccount_ = login->loginName;
        accountKnown_ = true;
      }
      log(F("PlusStore: VCSIDE is authorized for the account {0}.", {login->loginName}));
      return true;
    }
    if (polled.status != 0) {
      log(F("PlusStore: the authorization failed ({0}).", {httpProblem(polled)}));
      return false;
    }
  }
  log(cancel_ ? T("PlusStore: authorization cancelled.") : T("PlusStore: no answer from the browser page in 20 minutes: try again."));
  return false;
}

bool PlusStoreUploader::makeFolders(const PlusStoreLogin& login, std::string* error) {
  std::vector<std::string> parts = plusStoreFolderParts(settings_.folder);
  if (parts.empty()) {
    *error = F("the folder \"{0}\" is not valid: folder names separated by /, inside the account.", {settings_.folder});
    return false;
  }
  if (!settings_.createFolders) {
    Answer found = send("PROPFIND", plusStoreDavUrl(login, parts, ""), &login, "", {{"Depth", "0"}}, cancel_);
    if (found.status == 207) return true;
    if (found.status == 401) return false;
    *error = found.status == 404 ? F("the folder \"{0}\" does not exist in the account. Create it, or let VCSIDE create it (Settings > PlusStore).", {settings_.folder})
                                 : httpProblem(found);
    return false;
  }
  std::vector<std::string> prefix;
  for (auto& part : parts) {
    prefix.push_back(part);
    Answer made = send("MKCOL", plusStoreDavUrl(login, prefix, ""), &login, "", {}, cancel_);
    if (made.status == 201) {
      log(F("PlusStore: folder \"{0}\" created.", {[&prefix] {
            std::string joined;
            for (auto& name : prefix) joined += (joined.empty() ? "" : "/") + name;
            return joined;
          }()}));
    } else if (made.status != 405) {  // 405: it is already there
      if (made.status != 401) *error = F("cannot create the folder \"{0}\" ({1}).", {part, httpProblem(made)});
      return false;
    }
  }
  return true;
}

void PlusStoreUploader::uploadJob() {
  bool readOk = false;
  std::string contents = readWholeFile(path_, &readOk);
  if (!readOk || contents.empty()) {
    log(F("PlusStore: cannot read {0}: build the project first.", {path_}));
    return;
  }
  PlusStoreLogin login;
  std::string secret;
  if (assistant::loadFileSecret(kSecretAccount, &secret)) login = loginFromSecret(secret);
  for (int attempt = 0; attempt < 2 && !cancel_; attempt++) {
    if (!login.valid() && !authorize(&login)) return;
    std::string error;
    bool foldersReady = makeFolders(login, &error);
    if (foldersReady) {
      std::string url = plusStoreDavUrl(login, plusStoreFolderParts(settings_.folder), name_);
      Answer put = send("PUT", url, &login, contents, {{"Content-Type", "application/octet-stream"}}, cancel_);
      if (put.status == 201 || put.status == 204) {
        log(F("PlusStore: {0} uploaded to \"{1}\" ({2} bytes, {3}).",
              {name_, settings_.folder, std::to_string(contents.size()), put.status == 201 ? T("new") : T("replacing the one there")}));
        log(T("PlusStore: the PlusCart lists it in its menu, in that folder."));
        return;
      }
      if (put.status != 401) error = httpProblem(put);
    }
    if (!error.empty()) {
      log(F("PlusStore: upload failed: {0}", {error}));
      return;
    }
    // 401: the app password was revoked (on the website, or by another computer): authorize again
    log(T("PlusStore: the saved authorization is no longer valid. Authorizing again."));
    assistant::removeFileSecret(kSecretAccount);
    login = PlusStoreLogin();
  }
}

void PlusStoreUploader::signInJob() {
  PlusStoreLogin login;
  authorize(&login);
}

void PlusStoreUploader::signOutJob() {
  std::string secret;
  if (!assistant::loadFileSecret(kSecretAccount, &secret)) {
    log(T("PlusStore: VCSIDE was not authorized."));
    return;
  }
  PlusStoreLogin login = loginFromSecret(secret);
  if (login.valid()) {
    Answer revoked = send("DELETE", login.server + "/ocs/v2.php/core/apppassword", &login, "", {{"OCS-APIRequest", "true"}}, cancel_);
    if (revoked.status != 200 && revoked.status != 401)
      log(F("PlusStore: could not revoke the app password on the server ({0}): remove it in the PlusStore's Settings > Security.", {httpProblem(revoked)}));
  }
  assistant::removeFileSecret(kSecretAccount);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cachedAccount_.clear();
    accountKnown_ = true;
  }
  log(F("PlusStore: signed out of {0}. The next upload asks for the authorization again.", {login.loginName}));
}

}  // namespace vcs
