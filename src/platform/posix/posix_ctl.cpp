// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// POSIX (Linux host) control surface: a file-based emulation of the QNX resource manager.
//   <dir>/status, <dir>/events : rewritten atomically (temp file + rename) by publish()
//   <dir>/ctl                  : named FIFO; one service thread reads lines, answers each
//                                non-empty one by writing the reply atomically to <dir>/reply
// The service thread holds the FIFO open O_RDWR so it never observes EOF; poll() with a
// 100 ms timeout lets stop() join it promptly.
#define _POSIX_C_SOURCE 200809L
#include "relmw/platform.hpp"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace relmw::platform {
namespace {

// mkdir -p semantics: create every missing component of `path`.
bool ensure_dir(const std::string& path) {
  std::string p;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '/' && !p.empty()) {
      if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    p.push_back(path[i]);
  }
  if (!p.empty() && p != "/") {
    if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) return false;
  }
  return true;
}

// Publish `content` at `path` atomically: write a sibling temp file, then rename it over.
bool write_file_atomic(const std::string& path, const std::string& content) {
  const std::string tmp = path + ".tmp";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  size_t off = 0;
  while (off < content.size()) {
    const ssize_t w = write(fd, content.data() + off, content.size() - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      close(fd);
      unlink(tmp.c_str());
      return false;
    }
    off += (size_t)w;
  }
  if (close(fd) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace

struct ControlSurface::Impl {
  ControlSurface* self = nullptr;
  std::string ctl_path, reply_path, status_path, events_path;
  std::atomic<bool> running{false};
  bool started = false;
  pthread_t thread{};
  std::mutex mu;               // guards ctl_fd / fd_ready
  std::condition_variable cv;  // start() waits until the service thread holds the FIFO open
  bool fd_ready = false;
  int ctl_fd = -1;
  std::mutex pub_mu;           // serializes publish()'s temp+rename pairs
};

static void* service_thread(void* arg) {
  auto* im = static_cast<ControlSurface::Impl*>(arg);
  // O_RDWR: with our own reader end held open the FIFO never reports EOF.
  const int fd = open(im->ctl_path.c_str(), O_RDWR | O_CLOEXEC);
  {
    std::lock_guard<std::mutex> lk(im->mu);
    im->ctl_fd = fd;
    im->fd_ready = true;
    im->cv.notify_one();
  }
  if (fd < 0) return nullptr;
  std::string buf;
  char tmp[512];
  while (im->running.load(std::memory_order_acquire)) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int rc = poll(&pfd, 1, 100);  // 100 ms timeout so stop() is prompt
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0) continue;
    if (pfd.revents & (POLLERR | POLLNVAL)) break;
    if (!(pfd.revents & (POLLIN | POLLHUP))) continue;
    const ssize_t n = read(fd, tmp, sizeof tmp);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) continue;  // cannot happen with O_RDWR; be defensive
    buf.append(tmp, (size_t)n);
    for (size_t nl = buf.find('\n'); nl != std::string::npos; nl = buf.find('\n')) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      if (line.empty()) continue;
      const std::string reply = im->self->hooks_command(line) + "\n";
      write_file_atomic(im->reply_path, reply);
    }
  }
  close(fd);
  {
    std::lock_guard<std::mutex> lk(im->mu);
    im->ctl_fd = -1;
  }
  return nullptr;
}

ControlSurface::ControlSurface(std::string mount_point, ControlHooks hooks)
    : mount_(std::move(mount_point)), hooks_(std::move(hooks)), impl_(new Impl) {
  impl_->self = this;
  impl_->ctl_path = mount_ + "/ctl";
  impl_->reply_path = mount_ + "/reply";
  impl_->status_path = mount_ + "/status";
  impl_->events_path = mount_ + "/events";
}

ControlSurface::~ControlSurface() {
  stop();
  delete impl_;
}

bool ControlSurface::start(std::string* err) {
  if (impl_->started) return true;
  if (!ensure_dir(mount_)) {
    if (err) *err = "mkdir " + mount_ + ": " + strerror(errno);
    return false;
  }
  if (mkfifo(impl_->ctl_path.c_str(), 0666) != 0 && errno != EEXIST) {
    if (err) *err = "mkfifo " + impl_->ctl_path + ": " + strerror(errno);
    return false;
  }
  struct stat st{};
  if (stat(impl_->ctl_path.c_str(), &st) != 0 || !S_ISFIFO(st.st_mode)) {
    // Something exists at <dir>/ctl but it is not a FIFO: replace it.
    if (unlink(impl_->ctl_path.c_str()) != 0 && errno != ENOENT) {
      if (err) *err = "unlink " + impl_->ctl_path + ": " + strerror(errno);
      return false;
    }
    if (mkfifo(impl_->ctl_path.c_str(), 0666) != 0) {
      if (err) *err = "mkfifo " + impl_->ctl_path + ": " + strerror(errno);
      return false;
    }
  }
  impl_->running.store(true);
  if (pthread_create(&impl_->thread, nullptr, service_thread, impl_) != 0) {
    impl_->running.store(false);
    if (err) *err = "pthread_create failed";
    return false;
  }
  impl_->started = true;
  {
    std::unique_lock<std::mutex> lk(impl_->mu);
    impl_->cv.wait(lk, [im = impl_] { return im->fd_ready; });
  }
  if (impl_->ctl_fd < 0) {  // the FIFO could not be opened
    stop();
    if (err) *err = "open " + impl_->ctl_path + ": " + strerror(errno);
    return false;
  }
  publish();
  log(Level::Info, "control surface up at %s/{status,events,ctl,reply}", mount_.c_str());
  return true;
}

void ControlSurface::stop() {
  if (!impl_ || !impl_->started) return;  // idempotent
  impl_->running.store(false);
  pthread_join(impl_->thread, nullptr);
  impl_->started = false;
}

void ControlSurface::publish() {
  if (!impl_) return;
  std::lock_guard<std::mutex> lk(impl_->pub_mu);
  write_file_atomic(impl_->status_path, hooks_status());
  write_file_atomic(impl_->events_path, hooks_events());
}

}  // namespace relmw::platform
