// SPDX-License-Identifier: AGPL-3.0-or-later
#include "job.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;
namespace slicer {
fs::path new_job_directory() {
  auto cache = getenv("XDG_CACHE_HOME");
  auto home = getenv("HOME");
  fs::path root = cache ? cache : (home ? fs::path(home) / ".cache" : fs::temp_directory_path());
  root /= "orca-fltk/jobs";
  fs::create_directories(root);
  auto pattern = (root / "job-XXXXXX").string();
  std::vector<char> chars(pattern.begin(), pattern.end());
  chars.push_back(0);
  auto path = mkdtemp(chars.data());
  if (!path)
    throw std::runtime_error("Cannot create job directory");
  return path;
}
Job::~Job() {
  if (pid_ > 0) {
    kill(pid_, SIGTERM);
    for (int i = 0; i < 20; ++i) {
      if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
        pid_ = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      waitpid(pid_, nullptr, 0);
    }
  }
  if (fd_ >= 0)
    close(fd_);
}
void Job::start(const fs::path &worker, const fs::path &request) {
  if (busy())
    throw std::runtime_error("A slicing job is already running");
  if (access(worker.c_str(), X_OK))
    throw std::runtime_error("Slicing worker is unavailable: " + worker.string());
  int descriptors[2];
  if (pipe2(descriptors, O_CLOEXEC))
    throw std::runtime_error("Cannot create worker pipe");
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, descriptors[0]);
  posix_spawn_file_actions_addclose(&actions, descriptors[1]);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t mask;
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attr, &mask);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
  std::string executable = worker.string(), argument = request.string();
  char *args[] = {executable.data(), argument.data(), nullptr};
  auto result = posix_spawn(&pid_, executable.c_str(), &actions, &attr, args, environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  close(descriptors[1]);
  if (result) {
    close(descriptors[0]);
    pid_ = -1;
    throw std::runtime_error("Could not start slicing worker: " + std::to_string(result));
  }
  fd_ = descriptors[0];
  fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
  pending_.clear();
  cancelling_ = false;
}
void Job::cancel() {
  if (busy() && !cancelling_) {
    cancelling_ = true;
    cancelled_ = std::chrono::steady_clock::now();
    kill(pid_, SIGTERM);
  }
}
void Job::poll(const std::function<void(const std::string &)> &line,
               const std::function<void(int)> &done) {
  if (!busy())
    return;
  std::array<char, 8192> buffer;
  ssize_t n;
  while ((n = read(fd_, buffer.data(), buffer.size())) > 0) {
    pending_.append(buffer.data(), size_t(n));
    size_t pos;
    while ((pos = pending_.find('\n')) != std::string::npos) {
      line(pending_.substr(0, pos));
      pending_.erase(0, pos + 1);
    }
    if (pending_.size() > 1024 * 1024) {
      line(pending_);
      pending_.clear();
    }
  }
  if (cancelling_ && std::chrono::steady_clock::now() - cancelled_ > std::chrono::seconds(2))
    kill(pid_, SIGKILL);
  int status = 0;
  auto result = waitpid(pid_, &status, WNOHANG);
  if (result == pid_) {
    while ((n = read(fd_, buffer.data(), buffer.size())) > 0)
      pending_.append(buffer.data(), size_t(n));
    if (!pending_.empty()) {
      std::istringstream lines(pending_);
      std::string text;
      while (std::getline(lines, text))
        line(text);
      pending_.clear();
    }
    close(fd_);
    fd_ = -1;
    pid_ = -1;
    done(cancelling_ ? 130 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)));
  }
}
} // namespace slicer
