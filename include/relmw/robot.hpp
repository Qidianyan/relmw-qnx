// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// The only things the middleware needs from a robot / policy / verifier.
#pragma once
#include "relmw/types.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace relmw {

// Robot adapter (motion-level). Implemented by the deterministic 2-D simulator, a trajectory replayer,
// or (on a real cell) a driver process reached through QNX message passing.
class RobotAdapter {
 public:
  virtual ~RobotAdapter() = default;
  virtual Obs get_obs() = 0;                         // robot's own state estimate (policy input)
  virtual Obs step(Vec2 target_xy) = 0;              // one control tick toward target
  virtual Vec2 robot_pose() = 0;
  virtual void move_robot_to(Vec2 pose, int steps = 60) = 0;   // rollback primitive (robot only)
};

struct Perturb {
  bool has_seed = false;       // false on the first attempt (policy's nominal behaviour)
  std::uint64_t seed = 0;
  int retry = 0;
};

struct SubtaskMeta {
  int object = 0;              // index into Obs::obj
  Vec2 goal_xy;
};

// Any policy (ACT / diffusion / VLA / scripted). The middleware never looks inside it.
class Policy {
 public:
  virtual ~Policy() = default;
  virtual Vec2 act(const Obs& obs, const std::string& subtask, const SubtaskMeta& meta, const Perturb& p) = 0;
};

using VerifierFn = std::function<Verdict(const Obs&)>;

}  // namespace relmw
