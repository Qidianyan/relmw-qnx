// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
#include "relmw/strategies.hpp"
#include <stdexcept>

namespace relmw {

const char* const kStrategyOrder[7] = {"replay", "relocalize", "slow_mode", "obs_lead",
                                       "reapproach_left", "reapproach_right", "obs_jitter"};

namespace {

struct Replay : Strategy { std::string name() const override { return "replay"; } };

// Motion primitive: move to a laterally offset pre-contact pose behind the object, then hand back.
struct Reapproach : Strategy {
  double side, offset;
  Reapproach(double s, double off = 0.05) : side(s), offset(off) {}
  std::string name() const override { return side > 0 ? "reapproach_left" : "reapproach_right"; }
  void prepare(RobotAdapter& r, Vec2, std::optional<Vec2> o, Rng&, const SubtaskMeta& m) override {
    if (!o) return;
    Vec2 d = (m.goal_xy - *o).unit();
    r.move_robot_to(*o - d * 0.12 + d.perp() * (offset * side), 80);
    r.move_robot_to(*o - d * 0.07, 40);
  }
};

// Input perturbation: shift the perceived object back along the push direction (keeps pushing).
struct ObsLead : Strategy {
  double lead = 0.03; Vec2 bias; int key = 0;
  std::string name() const override { return "obs_lead"; }
  void prepare(RobotAdapter&, Vec2, std::optional<Vec2> o, Rng&, const SubtaskMeta& m) override {
    key = m.object;
    bias = o ? (m.goal_xy - *o).unit() * -lead : Vec2{};
  }
  Obs obs_fn(const Obs& o, Rng&) override { Obs r = o; r.obj[key] += bias; return r; }
};

// Sensor cross-check: replace the policy's own object estimate with the verifier camera's independent
// estimate (offset measured once at the rollback point). Fixes drifted perception (persistent cause).
struct Relocalize : Strategy {
  Vec2 corr; int key = 0;
  std::string name() const override { return "relocalize"; }
  void prepare(RobotAdapter& r, Vec2, std::optional<Vec2> o, Rng&, const SubtaskMeta& m) override {
    key = m.object;
    corr = o ? *o - r.get_obs().obj[key] : Vec2{};
  }
  Obs obs_fn(const Obs& o, Rng&) override { Obs r = o; r.obj[key] += corr; return r; }
};

// Randomised-smoothing style input perturbation.
struct ObsJitter : Strategy {
  double sigma = 0.012; Vec2 bias; int key = 0;
  std::string name() const override { return "obs_jitter"; }
  void prepare(RobotAdapter&, Vec2, std::optional<Vec2>, Rng& rng, const SubtaskMeta& m) override {
    key = m.object; bias = {rng.normal(0, sigma), rng.normal(0, sigma)};
  }
  Obs obs_fn(const Obs& o, Rng& rng) override {
    Obs r = o; r.obj[key] += bias + Vec2{rng.normal(0, 0.002), rng.normal(0, 0.002)}; return r;
  }
};

// Speed override (adapter level, like a teach-pendant speed slider).
struct SlowMode : Strategy {
  double scale = 0.5;
  std::string name() const override { return "slow_mode"; }
  Vec2 act_fn(Vec2 a, const Obs& o) override { return o.pusher + (a - o.pusher) * scale; }
};

}  // namespace

std::unique_ptr<Strategy> make_strategy(const std::string& n) {
  if (n == "replay") return std::make_unique<Replay>();
  if (n == "relocalize") return std::make_unique<Relocalize>();
  if (n == "slow_mode") return std::make_unique<SlowMode>();
  if (n == "obs_lead") return std::make_unique<ObsLead>();
  if (n == "reapproach_left") return std::make_unique<Reapproach>(+1);
  if (n == "reapproach_right") return std::make_unique<Reapproach>(-1);
  if (n == "obs_jitter") return std::make_unique<ObsJitter>();
  throw std::invalid_argument("unknown strategy: " + n);
}

}  // namespace relmw
