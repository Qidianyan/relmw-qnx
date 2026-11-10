// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
#include "relmw/verifier.hpp"
#include <cmath>

namespace relmw {

FailureMode classify(Vec2 obj, Vec2 pusher, Vec2 goal) {
  Vec2 to_goal = goal - obj;
  if ((pusher - obj).norm() > 0.12) return FailureMode::LostContact;
  if ((pusher - obj).dot(to_goal) > 0) return FailureMode::WrongSideStall;
  if (to_goal.norm() < 2 * kGoalTol) return FailureMode::StoppedShort;
  return FailureMode::OffAxisDrift;
}

static Verdict judge(Vec2 p, Vec2 pusher, int obj, double margin) {
  Verdict v;
  v.has_obj = true; v.obj_xy = p;
  Vec2 g = goal_of(obj);
  v.dist = (p - g).norm();
  if (std::fabs(p.x) > kTableX || std::fabs(p.y) > kTableY) { v.mode = FailureMode::Unrecoverable; return v; }
  if (v.dist < kGoalTol - margin) { v.ok = true; return v; }
  v.mode = classify(p, pusher, g);
  return v;
}

Verdict CameraVerifier::operator()(const Obs&) {
  Obs t = truth_->truth();
  Vec2 est = t.obj[obj_] + Vec2{rng_.normal(0, sigma_), rng_.normal(0, sigma_)};
  return judge(est, t.pusher, obj_, kVerifierMargin);
}

Verdict oracle_verdict(const Obs& truth, int obj) { return judge(truth.obj[obj], truth.pusher, obj, 0.0); }

}  // namespace relmw
