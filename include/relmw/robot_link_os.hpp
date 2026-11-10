// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// OS-specific entry points for the robot driver link (checklist C6). Kept free of POSIX/QNX
// includes so this header is portable; each platform provides the same two functions:
//   robot_connect() : open the client side of the link (returns null + err on failure)
//   robot_serve()   : run the server until `quit` is set; unlinks the socket on exit
#pragma once
#include "relmw/robot_link.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace relmw {

// Client side. POSIX: AF_UNIX SOCK_SEQPACKET socket `path`. QNX: connection to /dev/robot0.
std::unique_ptr<RobotTransport> robot_connect(const std::string& path, std::string* err);

// Server side: serves `server` at `path` until `quit` becomes true. Returns 0 on clean stop.
int robot_serve(const std::string& path, RobotServer& server, std::atomic<bool>& quit, std::string* err);

}  // namespace relmw
