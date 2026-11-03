// SPDX-License-Identifier: AGPL-3.0-or-later
#include "transfer.hpp"
#include <algorithm>
#include <curl/curl.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace slicer {
namespace {
void init() {
  static std::once_flag flag;
  std::call_once(flag, [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT))
      throw std::runtime_error("Cannot initialise HTTP transport");
  });
}
bool controls(const std::string &s) {
  return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
std::string endpoint(const UploadRequest &r) {
  init();
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
  if (!url || curl_url_set(url.get(), CURLUPART_URL, r.url.c_str(), 0))
    throw std::runtime_error("Enter a valid http:// or https:// print server URL");
  auto part = [&](CURLUPart p) {
    char *s = nullptr;
    auto code = curl_url_get(url.get(), p, &s, 0);
    std::string value = code == CURLUE_OK ? s : "";
    curl_free(s);
    return value;
  };
  if ((part(CURLUPART_SCHEME) != "http" && part(CURLUPART_SCHEME) != "https") ||
      part(CURLUPART_HOST).empty() || !part(CURLUPART_USER).empty() ||
      !part(CURLUPART_PASSWORD).empty() || !part(CURLUPART_QUERY).empty() ||
      !part(CURLUPART_FRAGMENT).empty() || controls(r.url))
    throw std::runtime_error("Use an HTTP(S) base URL without credentials, query or fragment");
  std::string result = r.url;
  while (!result.empty() && result.back() == '/')
    result.pop_back();
  return result + (r.protocol == "octoprint" ? "/api/files/local" : "/server/files/upload");
}
struct Response {
  std::string body;
};
size_t response(char *data, size_t size, size_t count, void *p) {
  auto &body = static_cast<Response *>(p)->body;
  size_t bytes = size * count;
  if (bytes > 1024 * 1024 - body.size())
    return 0;
  body.append(data, bytes);
  return bytes;
}
} // namespace
UploadStatus Transfer::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}
Transfer::~Transfer() {
  cancel();
  if (thread_.joinable())
    thread_.join();
}
void Transfer::validate(const UploadRequest &r) {
  if (r.protocol != "octoprint" && r.protocol != "moonraker")
    throw std::runtime_error("Unsupported print server protocol");
  if (!std::filesystem::is_regular_file(r.file) || !std::filesystem::file_size(r.file))
    throw std::runtime_error("Select an existing, non-empty G-code file");
  if (r.remote_name.empty() || r.remote_name == "." || r.remote_name == ".." ||
      r.remote_name.find_first_of("/\\") != std::string::npos || controls(r.remote_name))
    throw std::runtime_error("Remote filename must be a single filename");
  auto extension = std::filesystem::path(r.remote_name).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (extension != ".gcode" && extension != ".gco" && extension != ".g")
    throw std::runtime_error("Use a .gcode, .gco or .g remote filename");
  if (controls(r.api_key) || controls(r.folder) || r.folder.find('\\') != std::string::npos ||
      (!r.folder.empty() && r.folder.front() == '/'))
    throw std::runtime_error("Invalid API key or remote folder");
  for (const auto &part : std::filesystem::path(r.folder))
    if (part == ".." || part == ".")
      throw std::runtime_error("Remote folder must stay within the G-code directory");
  if (!r.ca_file.empty() && !std::filesystem::is_regular_file(r.ca_file))
    throw std::runtime_error("CA certificate file does not exist");
  (void)endpoint(r);
}
void Transfer::start(UploadRequest r) {
  if (status().running)
    throw std::runtime_error("A transfer is already running");
  validate(r);
  if (thread_.joinable())
    thread_.join();
  cancel_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = {true, false, false, 0, "Uploading G-code"};
  }
  try {
    thread_ = std::thread([this, r = std::move(r)]() mutable { run(std::move(r)); });
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.running = false;
    throw;
  }
}
void Transfer::run(UploadRequest r) {
  UploadStatus result;
  try {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
      throw std::runtime_error("Cannot create HTTP upload");
    std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime(curl_mime_init(curl.get()),
                                                               curl_mime_free);
    if (!mime)
      throw std::runtime_error("Cannot prepare upload form");
    auto field = [&](const char *name, const std::string &value) {
      auto part = curl_mime_addpart(mime.get());
      if (!part || curl_mime_name(part, name) || curl_mime_data(part, value.c_str(), value.size()))
        throw std::runtime_error("Cannot prepare upload form");
    };
    auto file = curl_mime_addpart(mime.get());
    if (!file || curl_mime_name(file, "file") || curl_mime_filedata(file, r.file.c_str()) ||
        curl_mime_filename(file, r.remote_name.c_str()) ||
        curl_mime_type(file, "application/octet-stream"))
      throw std::runtime_error("Cannot read G-code for upload");
    field("print", "false");
    if (r.protocol == "octoprint")
      field("select", "false");
    else
      field("root", "gcodes");
    if (!r.folder.empty())
      field("path", r.folder);
    curl_slist *headers = nullptr;
    if (!r.api_key.empty())
      headers = curl_slist_append(headers, ("X-Api-Key: " + r.api_key).c_str());
    if (!r.api_key.empty() && !headers)
      throw std::runtime_error("Cannot prepare upload authentication");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owner(headers, curl_slist_free_all);
    Response received;
    auto url = endpoint(r);
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 1800L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    if (!r.ca_file.empty())
      curl_easy_setopt(curl.get(), CURLOPT_CAINFO, r.ca_file.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, response);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &received);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        curl.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void *p, curl_off_t, curl_off_t, curl_off_t total, curl_off_t now) -> int {
          auto self = static_cast<Transfer *>(p);
          {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->status_.percent =
                total > 0 ? std::min(99.0, 100.0 * double(now) / double(total)) : 0;
          }
          return self->cancel_ ? 1 : 0;
        });
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, this);
    auto code = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (cancel_) {
      result.cancelled = true;
      result.message = "Transfer cancelled; the server may retain a received file";
    } else if (code != CURLE_OK)
      throw std::runtime_error(std::string("Upload failed: ") + curl_easy_strerror(code));
    else if (status < 200 || status >= 300)
      throw std::runtime_error("Upload rejected (HTTP " + std::to_string(status) +
                               "). Check server URL, API key and upload permission");
    else {
      auto json = nlohmann::json::parse(received.body, nullptr, false);
      bool accepted = !json.is_discarded() && json.is_object() && !json.contains("error");
      if (r.protocol == "octoprint")
        accepted = accepted && json.contains("files") && json["files"].is_object() &&
                   json["files"].contains("local");
      else
        accepted = accepted && json.contains("item") && json["item"].is_object() &&
                   json["item"].contains("path");
      if (!accepted)
        throw std::runtime_error(
            "Server returned an unexpected upload response; check the server before retrying");
      result.success = true;
      result.percent = 100;
      result.message = "G-code uploaded; printing was not started";
    }
  } catch (const std::exception &e) {
    result.message = e.what();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = std::move(result);
}
} // namespace slicer
