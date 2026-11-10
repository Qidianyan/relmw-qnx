// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Portable robot-driver wire protocol (checklist C6): fixed-size request/reply messages shared by
// the supervisor (client) and relmw_robotd (server, in-process loopback or a separate process).
//
// One request -> one reply, synchronous, exactly like QNX MsgSend/MsgReceive/MsgReply:
//
//   struct _io_msg { uint16_t type, combine_len, mgrid, subtype; }   <- QNX message header
//   struct ProtoHdr { uint16_t type, combine_len, mgrid, subtype; }  <- same layout, portable
//
// The QNX transport fills type/mgrid with the real QNX constants (_IO_CONNECT etc. and the manager
// id); portable code sets type = 0, combine_len = sizeof(ProtoHdr), mgrid = kRobotMgrId. subtype
// always carries the RobotOp op code. No OS headers are included here so the same layout is used
// on every target; the structs are trivially copyable, so they can be memcpy'd straight into a
// message buffer.
#pragma once
#include "relmw/types.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace relmw {

// "_IOMGR_PRIVATE_BASE + 0x52": private manager id of the robot driver (QNX: resmgr_attach range).
constexpr std::uint16_t kRobotMgrId = 0x0100 + 0x52;
constexpr std::uint32_t kRobotProtoVersion = 1;

// First 8 bytes of every request: byte-for-byte the layout of QNX struct _io_msg.
struct ProtoHdr {
  std::uint16_t type;        // QNX _IO_MSG type (0 on the portable path)
  std::uint16_t combine_len; // sizeof(ProtoHdr) when not combined
  std::uint16_t mgrid;       // manager id: kRobotMgrId
  std::uint16_t subtype;     // op code (RobotOp)
};

enum class RobotOp : std::uint16_t {
  Hello = 1,
  Reset,
  GetObs,
  Step,
  Pose,
  MoveTo,
  Truth,
  Knock,
  Status,
};

struct RobotRequest {
  ProtoHdr hdr;
  std::uint32_t version;
  std::uint32_t seq;
  std::int32_t i[4];
  double d[4];
};

// status 0 = OK; else an errno-style positive code:
//   EINVAL bad op / bad args / short message / non-finite double,
//   EPROTO version mismatch, ENODEV op before a Reset.
struct RobotReply {
  std::int32_t status;
  std::uint32_t seq;
  double obs[2 + 2 * kNumObjects];   // pusher xy + one xy per object
  double pose[2];
  std::int64_t ticks;
  std::int32_t fault;
  std::int32_t pad;
};

static_assert(std::is_trivially_copyable<ProtoHdr>::value, "ProtoHdr must be trivially copyable");
static_assert(std::is_standard_layout<ProtoHdr>::value, "ProtoHdr must be standard layout");
static_assert(std::is_trivially_copyable<RobotRequest>::value, "RobotRequest must be trivially copyable");
static_assert(std::is_standard_layout<RobotRequest>::value, "RobotRequest must be standard layout");
static_assert(std::is_trivially_copyable<RobotReply>::value, "RobotReply must be trivially copyable");
static_assert(std::is_standard_layout<RobotReply>::value, "RobotReply must be standard layout");

// Fixed sizes / offsets: no implicit padding surprises on any target we build for.
static_assert(sizeof(ProtoHdr) == 8, "ProtoHdr must be the 8-byte _io_msg header");
static_assert(sizeof(RobotRequest) == 64, "RobotRequest is a fixed 64-byte message");
static_assert(sizeof(RobotReply) == 88, "RobotReply is a fixed 88-byte message");
static_assert(offsetof(RobotRequest, hdr) == 0, "request header at offset 0");
static_assert(offsetof(RobotRequest, version) == 8, "version at offset 8");
static_assert(offsetof(RobotRequest, seq) == 12, "seq at offset 12");
static_assert(offsetof(RobotRequest, i) == 16, "i[] at offset 16");
static_assert(offsetof(RobotRequest, d) == 32, "d[] at offset 32");
static_assert(offsetof(RobotReply, status) == 0, "reply status at offset 0");
static_assert(offsetof(RobotReply, seq) == 4, "reply seq at offset 4");
static_assert(offsetof(RobotReply, obs) == 8, "reply obs at offset 8");
static_assert(offsetof(RobotReply, pose) == 56, "reply pose at offset 56");
static_assert(offsetof(RobotReply, ticks) == 72, "reply ticks at offset 72");
static_assert(offsetof(RobotReply, fault) == 80, "reply fault at offset 80");
static_assert(offsetof(RobotReply, pad) == 84, "reply pad at offset 84");

constexpr std::int32_t kRobotStatusOk = 0;

inline RobotRequest make_request(RobotOp op, std::uint32_t seq) {
  RobotRequest r{};                       // zero everything (pad bytes included)
  r.hdr.type = 0;                         // portable path; QNX fills the real type
  r.hdr.combine_len = sizeof(ProtoHdr);
  r.hdr.mgrid = kRobotMgrId;
  r.hdr.subtype = static_cast<std::uint16_t>(op);
  r.version = kRobotProtoVersion;
  r.seq = seq;
  return r;
}

// memcpy of doubles: bit-exact in both directions (no reformatting, no decimal round-trip).
inline void obs_to_wire(const Obs& o, double* w) {
  w[0] = o.pusher.x; w[1] = o.pusher.y;
  for (int i = 0; i < kNumObjects; ++i) { w[2 + 2 * i] = o.obj[i].x; w[3 + 2 * i] = o.obj[i].y; }
}

inline Obs obs_from_wire(const double* w) {
  Obs o;
  o.pusher.x = w[0]; o.pusher.y = w[1];
  for (int i = 0; i < kNumObjects; ++i) { o.obj[i].x = w[2 + 2 * i]; o.obj[i].y = w[3 + 2 * i]; }
  return o;
}

}  // namespace relmw
