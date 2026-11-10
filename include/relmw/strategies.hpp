// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Recovery strategies. All are policy-agnostic: they act only through the robot adapter (motion
// primitives / speed override), the observation stream, or the action stream. Never policy internals.
#pragma once
#include "relmw/robot.hpp"
#include "relmw/rng.hpp"
#include <memory>
#include <optional>
#include <string>

namespace relmw {

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual std::string name() const = 0;
  // Called after the middleware rolled the robot back to start_pose.
  virtual void prepare(RobotAdapter&, Vec2 /*start_pose*/, std::optional<Vec2> /*start_obj*/, Rng&, const SubtaskMeta&) {}
  virtual Obs obs_fn(const Obs& o, Rng&) { return o; }
  virtual Vec2 act_fn(Vec2 act, const Obs&) { return act; }
};

// Known strategy names: replay, relocalize, slow_mode, obs_lead, reapproach_left, reapproach_right, obs_jitter
std::unique_ptr<Strategy> make_strategy(const std::string& name);
extern const char* const kStrategyOrder[7];

}  // namespace relmw
