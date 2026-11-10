// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Robot driver link (checklist C6): a RobotServer owns the robot (the 2-D push simulator) and the
// overhead-camera ground truth; clients (RemoteRobot) talk to it over a RobotTransport with the
// fixed-size request/reply messages defined in relmw/robot_proto.hpp. This header is OS-neutral:
// the POSIX and QNX transports both provide robot_connect()/robot_serve() (see robot_link_os.hpp).
#pragma once
#include "relmw/robot_proto.hpp"
#include "relmw/sim2d.hpp"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

namespace relmw {

// In-process or remote server side. handle() validates the request, executes the op against the
// owned PushSim2D and fills the reply. No allocation on any path except Reset (which owns the sim).
class RobotServer {
 public:
  void handle(const RobotRequest& req, size_t req_len, RobotReply& reply);
  // One line: robot0 episode=<seed> fault=<name> ticks=<n> requests=<n> errors=<n>
  std::string status_text() const;
  long requests() const { return requests_.load(std::memory_order_relaxed); }   // read by robotd's watcher thread
  long errors() const { return errors_.load(std::memory_order_relaxed); }
 private:
  std::unique_ptr<PushSim2D> sim_;              // null until the first Reset
  std::uint64_t seed_ = 0;
  std::atomic<long> requests_{0};               // atomic: --quit-after-requests watcher reads it concurrently
  std::atomic<long> errors_{0};
};

// One request -> one reply, synchronous. Returns 0 or an errno value on transport failure.
class RobotTransport {
 public:
  virtual ~RobotTransport() = default;
  virtual int call(const RobotRequest&, RobotReply&) = 0;
};

// Loopback transport: calls RobotServer::handle directly (tests, in-process supervisor).
class LoopbackTransport : public RobotTransport {
 public:
  explicit LoopbackTransport(RobotServer& s) : server_(s) {}
  int call(const RobotRequest& req, RobotReply& reply) override {
    server_.handle(req, sizeof(RobotRequest), reply);
    return 0;
  }
 private:
  RobotServer& server_;
};

// Transport failure, non-zero reply status, or protocol (seq) mismatch.
struct RobotLinkError : std::runtime_error {
  int code;   // errno-style code
  RobotLinkError(const std::string& msg, int c) : std::runtime_error(msg), code(c) {}
};

// Client side of the robot link: a RobotAdapter + TruthSensor over any RobotTransport. hello()
// runs in the constructor (throws RobotLinkError on version mismatch). Every call increments seq
// and checks the echo; transport errors / non-zero status / seq mismatch throw RobotLinkError.
// The success path performs no allocation (stack-only request/reply structs).
class RemoteRobot : public RobotAdapter, public TruthSensor {
 public:
  explicit RemoteRobot(RobotTransport& t) : transport_(t) { hello(); }
  Fault reset(std::uint64_t seed, const SimConfig& cfg);
  Obs get_obs() override;
  Obs step(Vec2 target_xy) override;
  Vec2 robot_pose() override;
  void move_robot_to(Vec2 pose, int steps = 60) override;
  Obs truth() override;
  void knock(int obj, Vec2 delta);
  long ticks();
  long last_ticks() const { return last_ticks_; }   // ticks from the most recent reply (no IPC)
 private:
  void hello();
  RobotReply send(RobotRequest& req);   // transport round-trip + echo-seq/status checks (throws)
  RobotTransport& transport_;
  std::uint32_t seq_ = 0;
  long last_ticks_ = 0;
};

}  // namespace relmw
