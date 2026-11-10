// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// relmw_robotd: the robot driver process (checklist C6). It owns the robot (the deterministic 2-D
// push simulator) and the "overhead camera" ground truth, and answers fixed-size request/reply
// messages over the robot link (POSIX: AF_UNIX SOCK_SEQPACKET; QNX: /dev/robot0 resmgr, wired by
// the reviewer). One request -> one reply, synchronous, like QNX MsgSend/MsgReceive/MsgReply.
//
//   relmw_robotd [--path P] [--quit-after-requests N]
//
// Default path: /tmp/relmw_robot0.sock on POSIX (/dev/robot0 on QNX). SIGINT/SIGTERM -> clean exit
// (socket unlinked). --quit-after-requests N = exit(3) after N requests (crash simulation for the
// supervisor recovery tests).
#include "relmw/platform.hpp"
#include "relmw/robot_link.hpp"
#include "relmw/robot_link_os.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

using namespace relmw;
namespace pf = relmw::platform;

namespace {
std::atomic<bool> g_quit{false};
extern "C" void on_signal(int) { g_quit.store(true); }

#ifdef __QNX__
const char* kDefaultPath = "/dev/robot0";
#else
const char* kDefaultPath = "/tmp/relmw_robot0.sock";
#endif
}  // namespace

int main(int argc, char** argv) {
  std::string path = kDefaultPath;
  long quit_after = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
    if (a == "--path") path = next();
    else if (a == "--quit-after-requests") quit_after = std::atol(next().c_str());
    else { std::fprintf(stderr, "usage: %s [--path P] [--quit-after-requests N]\n", argv[0]); return 2; }
  }
  pf::log_init("relmw_robotd");
  pf::log(pf::Level::Info, "relmw_robotd start: path=%s quit_after=%ld", path.c_str(), quit_after);

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  RobotServer server;
  std::string err;
  if (quit_after > 0) {
    // Crash simulation: a side thread watches the request count and kills the process with _exit(3) at N — no
    // cleanup, no reply to the request in flight, exactly what the supervisor sees when a real driver dies (and
    // the serve loop does not have to be woken up, which on QNX would need a message or a signal).
    std::thread([&server, quit_after] {
      for (;;) {
        if (server.requests() >= quit_after) {
          pf::log(pf::Level::Warn, "relmw_robotd: --quit-after-requests %ld reached, simulated crash", quit_after);
          std::_Exit(3);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }).detach();
  }
  const int rc = robot_serve(path, server, g_quit, &err);
  if (rc != 0) {
    pf::log(pf::Level::Error, "relmw_robotd serve failed: %s", err.c_str());
    return 1;
  }
  pf::log(pf::Level::Info, "relmw_robotd exit: %s", server.status_text().c_str());
  return 0;
}
