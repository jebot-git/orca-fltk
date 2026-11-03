// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
namespace slicer {
struct UploadRequest {
  std::filesystem::path file;
  std::string protocol, url, api_key, remote_name, folder, ca_file;
};
struct UploadStatus {
  bool running = false, success = false, cancelled = false;
  double percent = 0;
  std::string message;
};
class Transfer {
  std::thread thread_;
  std::atomic<bool> cancel_{false};
  mutable std::mutex mutex_;
  UploadStatus status_;
  void run(UploadRequest request);

public:
  ~Transfer();
  void start(UploadRequest request);
  void cancel() { cancel_ = true; }
  UploadStatus status() const;
  static void validate(const UploadRequest &request);
};
} // namespace slicer
