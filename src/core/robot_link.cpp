// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Robot driver link: server side (owns the PushSim2D + ground truth) and the RemoteRobot client.
#include "relmw/robot_link.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace relmw {
namespace {

constexpr std::int32_t kOk = 0;
constexpr std::int32_t kMaxMoveSteps = 10000;   // rollback uses 60; a request must not hold the driver for long

bool finite(const double* d, int n) {
  for (int k = 0; k < n; ++k)
    if (!std::isfinite(d[k])) return false;
  return true;
}

}  // namespace

void RobotServer::handle(const RobotRequest& req, size_t req_len, RobotReply& reply) {
  std::memset(&reply, 0, sizeof reply);
  reply.seq = req.seq;                      // every reply echoes seq, ticks and fault
  reply.status = EINVAL;
  ++requests_;
  if (req_len < sizeof(RobotRequest)) { ++errors_; return; }
  if (req.hdr.mgrid != kRobotMgrId) { ++errors_; return; }
  if (req.version != kRobotProtoVersion) { reply.status = EPROTO; ++errors_; return; }
  const std::uint16_t op = req.hdr.subtype;
  if (op < static_cast<std::uint16_t>(RobotOp::Hello) || op > static_cast<std::uint16_t>(RobotOp::Status)) {
    ++errors_;
    return;
  }
  if (op == static_cast<std::uint16_t>(RobotOp::Hello)) { reply.status = kOk; return; }
  if (op == static_cast<std::uint16_t>(RobotOp::Reset)) {
    if (req.i[2] != 0 && req.i[2] != 1) { ++errors_; return; }
    if (req.i[3] < -1 || req.i[3] > 3) { ++errors_; return; }
    const std::uint64_t seed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(req.i[1])) << 32) |
                               static_cast<std::uint32_t>(req.i[0]);
    SimConfig cfg;
    cfg.field_faults = req.i[2] != 0;
    cfg.force_fault = req.i[3];
    sim_ = std::make_unique<PushSim2D>(seed, cfg);   // the only allocation in handle()
    seed_ = seed;
    reply.fault = static_cast<std::int32_t>(sim_->fault());
    reply.ticks = sim_->ticks();
    reply.status = kOk;
    return;
  }
  if (!sim_) { reply.status = ENODEV; ++errors_; return; }   // every op below needs a Reset first
  switch (static_cast<RobotOp>(op)) {
    case RobotOp::GetObs:
      obs_to_wire(sim_->get_obs(), reply.obs);
      break;
    case RobotOp::Step:
      if (!finite(req.d, 2)) { ++errors_; reply.status = EINVAL; return; }
      obs_to_wire(sim_->step(Vec2{req.d[0], req.d[1]}), reply.obs);
      break;
    case RobotOp::Pose:
      reply.pose[0] = sim_->robot_pose().x;
      reply.pose[1] = sim_->robot_pose().y;
      break;
    case RobotOp::MoveTo:
      if (!finite(req.d, 2)) { ++errors_; reply.status = EINVAL; return; }
      if (req.i[0] < 0 || req.i[0] > kMaxMoveSteps) { ++errors_; reply.status = EINVAL; return; }   // bounded work per request
      sim_->move_robot_to(Vec2{req.d[0], req.d[1]}, req.i[0]);
      reply.pose[0] = sim_->robot_pose().x;
      reply.pose[1] = sim_->robot_pose().y;
      break;
    case RobotOp::Truth:
      obs_to_wire(sim_->truth(), reply.obs);
      break;
    case RobotOp::Knock:
      if (req.i[0] < 0 || req.i[0] >= kNumObjects) { ++errors_; reply.status = EINVAL; return; }
      if (!finite(req.d, 2)) { ++errors_; reply.status = EINVAL; return; }
      sim_->knock(req.i[0], Vec2{req.d[0], req.d[1]});
      break;
    case RobotOp::Status:
      break;
    default:
      return;   // unreachable: op range validated above
  }
  reply.ticks = sim_->ticks();                       // every reply carries ticks + fault
  reply.fault = static_cast<std::int32_t>(sim_->fault());
  reply.status = kOk;
}

std::string RobotServer::status_text() const {
  char b[160];
  std::snprintf(b, sizeof b, "robot0 episode=%llu fault=%s ticks=%ld requests=%ld errors=%ld",
                static_cast<unsigned long long>(seed_), sim_ ? to_string(sim_->fault()) : "none",
                sim_ ? sim_->ticks() : 0L, requests(), errors());
  return b;
}

RobotReply RemoteRobot::send(RobotRequest& req) {
  const std::uint32_t expect = req.seq;
  RobotReply rep{};
  if (int rc = transport_.call(req, rep))
    throw RobotLinkError("robot op " + std::to_string(req.hdr.subtype) + ": transport error " + std::to_string(rc), rc);
  if (rep.seq != expect) throw RobotLinkError("robot op: seq echo mismatch", EPROTO);
  if (rep.status != kOk)
    throw RobotLinkError("robot op " + std::to_string(req.hdr.subtype) + ": status " + std::to_string(rep.status),
                         rep.status ? rep.status : EPROTO);
  last_ticks_ = rep.ticks;   // every reply carries sim.ticks(): free ticks for the caller
  return rep;
}

void RemoteRobot::hello() {
  RobotRequest req = make_request(RobotOp::Hello, seq_++);
  send(req);   // server answers EPROTO on version mismatch
}

Fault RemoteRobot::reset(std::uint64_t seed, const SimConfig& cfg) {
  RobotRequest req = make_request(RobotOp::Reset, seq_++);
  req.i[0] = static_cast<std::int32_t>(static_cast<std::uint32_t>(seed & 0xFFFFFFFFu));
  req.i[1] = static_cast<std::int32_t>(static_cast<std::uint32_t>(seed >> 32));
  req.i[2] = cfg.field_faults ? 1 : 0;
  req.i[3] = cfg.force_fault;
  RobotReply rep = send(req);
  return static_cast<Fault>(rep.fault);
}

Obs RemoteRobot::get_obs() {
  RobotRequest req = make_request(RobotOp::GetObs, seq_++);
  RobotReply rep = send(req);
  return obs_from_wire(rep.obs);
}

Obs RemoteRobot::step(Vec2 target_xy) {
  RobotRequest req = make_request(RobotOp::Step, seq_++);
  req.d[0] = target_xy.x;
  req.d[1] = target_xy.y;
  RobotReply rep = send(req);
  return obs_from_wire(rep.obs);
}

Vec2 RemoteRobot::robot_pose() {
  RobotRequest req = make_request(RobotOp::Pose, seq_++);
  RobotReply rep = send(req);
  return Vec2{rep.pose[0], rep.pose[1]};
}

void RemoteRobot::move_robot_to(Vec2 pose, int steps) {
  RobotRequest req = make_request(RobotOp::MoveTo, seq_++);
  req.d[0] = pose.x;
  req.d[1] = pose.y;
  req.i[0] = steps;
  send(req);
}

Obs RemoteRobot::truth() {
  RobotRequest req = make_request(RobotOp::Truth, seq_++);
  RobotReply rep = send(req);
  return obs_from_wire(rep.obs);
}

void RemoteRobot::knock(int obj, Vec2 delta) {
  RobotRequest req = make_request(RobotOp::Knock, seq_++);
  req.i[0] = obj;
  req.d[0] = delta.x;
  req.d[1] = delta.y;
  send(req);
}

long RemoteRobot::ticks() {
  RobotRequest req = make_request(RobotOp::Status, seq_++);
  RobotReply rep = send(req);
  return static_cast<long>(rep.ticks);
}

}  // namespace relmw
