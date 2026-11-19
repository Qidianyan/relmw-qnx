// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// POSIX transport for the robot driver link: one AF_UNIX SOCK_SEQPACKET socket (message boundaries,
// like QNX messages). One client at a time is enough. The serve loop polls with a 100 ms timeout so
// `quit` is honoured; the socket is unlinked on start (stale) and on exit. The client sends one
// fixed-size request and reads one fixed-size reply; transport failures return the errno (never
// SIGPIPE: sends use MSG_NOSIGNAL; the reply is polled with a 2 s timeout).
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "relmw/robot_link_os.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace relmw {
namespace {

#ifdef __APPLE__
constexpr int kRobotSockType = SOCK_STREAM;
constexpr int kSendNoSigPipe = 0;
inline void disable_sigpipe(int fd) {
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
}
#else
constexpr int kRobotSockType = SOCK_SEQPACKET;
constexpr int kSendNoSigPipe = MSG_NOSIGNAL;
inline void disable_sigpipe(int) {}
#endif

class PosixRobotTransport : public RobotTransport {
 public:
  explicit PosixRobotTransport(int fd) : fd_(fd) {}
  ~PosixRobotTransport() override { if (fd_ >= 0) ::close(fd_); }
  PosixRobotTransport(const PosixRobotTransport&) = delete;
  PosixRobotTransport& operator=(const PosixRobotTransport&) = delete;
  int call(const RobotRequest& req, RobotReply& rep) override;

 private:
  int fd_;
};

int PosixRobotTransport::call(const RobotRequest& req, RobotReply& rep) {
  const char* rq = reinterpret_cast<const char*>(&req);
  size_t left = sizeof req;
  while (left > 0) {                       // request: full message or errno
    const ssize_t n = ::send(fd_, rq, left, kSendNoSigPipe);
    if (n < 0) { if (errno == EINTR) continue; return errno ? errno : EIO; }
    if (n == 0) return EPIPE;
    rq += n; left -= static_cast<size_t>(n);
  }
  pollfd pfd{};
  pfd.fd = fd_; pfd.events = POLLIN;
  int pr;
  do { pr = ::poll(&pfd, 1, 2000); } while (pr < 0 && errno == EINTR);   // reply timeout: 2 s
  if (pr < 0) return errno ? errno : EIO;
  if (pr == 0) return ETIMEDOUT;
  char* rp = reinterpret_cast<char*>(&rep);
  left = sizeof rep;
  while (left > 0) {                       // reply: full message or errno
    const ssize_t n = ::recv(fd_, rp, left, 0);
    if (n < 0) { if (errno == EINTR) continue; return errno ? errno : EIO; }
    if (n == 0) return ECONNRESET;         // driver gone: short read / EOF
    rp += n; left -= static_cast<size_t>(n);
  }
  return 0;
}

bool sockaddr_for(const std::string& path, sockaddr_un* out, std::string* err) {
  if (path.size() >= sizeof(out->sun_path)) {
    if (err) *err = "robot socket path too long: " + path;
    return false;
  }
  std::memset(out, 0, sizeof *out);
  out->sun_family = AF_UNIX;
  std::strncpy(out->sun_path, path.c_str(), sizeof out->sun_path - 1);
  return true;
}

}  // namespace

std::unique_ptr<RobotTransport> robot_connect(const std::string& path, std::string* err) {
  const int fd = ::socket(AF_UNIX, kRobotSockType, 0);
  if (fd < 0) { if (err) *err = std::strerror(errno); return nullptr; }
  disable_sigpipe(fd);
  sockaddr_un addr{};
  if (!sockaddr_for(path, &addr, err)) { ::close(fd); return nullptr; }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    if (err) *err = std::strerror(errno) + std::string(" (") + path + ")";
    ::close(fd);
    return nullptr;
  }
  return std::make_unique<PosixRobotTransport>(fd);
}

int robot_serve(const std::string& path, RobotServer& server, std::atomic<bool>& quit, std::string* err) {
  sockaddr_un addr{};
  if (!sockaddr_for(path, &addr, err)) return -1;
  const int lfd = ::socket(AF_UNIX, kRobotSockType, 0);
  if (lfd < 0) { if (err) *err = std::strerror(errno); return -1; }
  disable_sigpipe(lfd);
  ::unlink(path.c_str());                  // stale socket from a previous run
  if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    if (err) *err = std::strerror(errno) + std::string(" (") + path + ")";
    ::close(lfd);
    return -1;
  }
  if (::listen(lfd, 1) < 0) {
    if (err) *err = std::strerror(errno);
    ::close(lfd);
    ::unlink(path.c_str());
    return -1;
  }
  int cfd = -1;
  for (;;) {
    if (quit.load()) break;
    pollfd pfd{};
    pfd.fd = cfd >= 0 ? cfd : lfd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, 100);   // 100 ms: `quit` is honoured promptly
    if (pr < 0) { if (errno == EINTR) continue; if (err) *err = std::strerror(errno); break; }
    if (pr == 0) continue;
    if (cfd < 0) {
      cfd = ::accept(lfd, nullptr, nullptr);
      if (cfd < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; if (err) *err = std::strerror(errno); break; }
      disable_sigpipe(cfd);
      continue;
    }
    RobotRequest req{};
    char* rp = reinterpret_cast<char*>(&req);
    size_t left = sizeof req;
    bool dead = false;
    while (left > 0) {
      const ssize_t n = ::recv(cfd, rp, left, 0);
      if (n < 0) { if (errno == EINTR) continue; dead = true; break; }
      if (n == 0) { dead = true; break; }  // client gone
      rp += n; left -= static_cast<size_t>(n);
    }
    if (dead) { ::close(cfd); cfd = -1; continue; }
    RobotReply rep{};
    server.handle(req, sizeof req, rep);
    const char* wp = reinterpret_cast<const char*>(&rep);
    size_t wleft = sizeof rep;
    while (wleft > 0) {
      const ssize_t n = ::send(cfd, wp, wleft, kSendNoSigPipe);
      if (n < 0) { if (errno == EINTR) continue; dead = true; break; }
      if (n == 0) { dead = true; break; }
      wp += n; wleft -= static_cast<size_t>(n);
    }
    if (dead) { ::close(cfd); cfd = -1; }
  }
  if (cfd >= 0) ::close(cfd);
  ::close(lfd);
  ::unlink(path.c_str());                  // unlink on exit
  return 0;
}

}  // namespace relmw
