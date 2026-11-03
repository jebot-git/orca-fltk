// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "profiles.hpp"
#include <chrono>
#include <functional>
#include <sys/types.h>
namespace slicer {
class Job {
  pid_t pid_ = -1;
  int fd_ = -1;
  std::string pending_;
  bool cancelling_ = false;
  std::chrono::steady_clock::time_point cancelled_;

public:
  ~Job();
  bool busy() const { return pid_ > 0; }
  void start(const fs::path &worker, const fs::path &request);
  void cancel();
  void poll(const std::function<void(const std::string &)> &line,
            const std::function<void(int)> &done);
};
fs::path new_job_directory();
} // namespace slicer
