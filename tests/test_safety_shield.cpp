// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Unit & invariant tests for the 1 kHz Simplex Physical Safety Shield (safety_shield.hpp).
#include "minitest.hpp"
#include "relmw/experiment.hpp"
#include "relmw/policy.hpp"
#include "relmw/retry_policy.hpp"
#include "relmw/safety_shield.hpp"
#include "relmw/verifier.hpp"
#include <cmath>
#include <limits>

using namespace relmw;

namespace {
struct PulseCounter {
  std::uint64_t calls = 0;
  std::uint32_t last_mask = 0;
  std::uint64_t last_total = 0;
};

void on_shield_pulse(void* ctx, std::uint32_t mask, std::uint64_t total) {
  auto* pc = static_cast<PulseCounter*>(ctx);
  ++pc->calls;
  pc->last_mask = mask;
  pc->last_total = total;
}
}  // namespace

TEST(shield_sanitizes_nan_and_inf_and_fires_pulse) {
  SafetyShield shield(SafetyEnvelope::medical_surgical_fixture());
  PulseCounter pc;
  shield.set_pulse_hook(&on_shield_pulse, &pc);

  const Vec2 cur{0.0, -0.30};
  const ShieldDecision d1 = shield.project(cur, Vec2{std::numeric_limits<double>::quiet_NaN(), 0.1});
  CHECK(d1.intervened());
  CHECK((d1.reason_mask & kShieldNonFinite) != 0);
  CHECK(d1.safe_target.x == cur.x && d1.safe_target.y == cur.y);

  const ShieldDecision d2 = shield.project(cur, Vec2{0.0, std::numeric_limits<double>::infinity()});
  CHECK(d2.intervened());
  CHECK((d2.reason_mask & kShieldNonFinite) != 0);
  CHECK(pc.calls == 2);
  CHECK(pc.last_total == 2);
  CHECK(shield.stats().nonfinite_clamps == 2);
}

TEST(shield_enforces_velocity_and_acceleration_limits) {
  SafetyEnvelope env = SafetyEnvelope::industrial_default();
  env.max_step_delta = 0.04;
  env.max_step_accel = 0.02;
  SafetyShield shield(env);

  const Vec2 p0{0.0, -0.20};
  // Desired jump of 0.30 m -> clamped to 0.04 m velocity step
  const ShieldDecision d1 = shield.project(p0, Vec2{0.30, -0.20});
  CHECK((d1.reason_mask & kShieldVelocityLimit) != 0);
  CHECK(std::fabs((d1.safe_target - p0).norm() - 0.04) < 1e-9);

  // Sudden reversal (-0.30, -0.20): velocity clamp gives (-0.04, 0), dv from (+0.04, 0) is 0.08 > 0.02 accel limit!
  const Vec2 p1 = d1.safe_target;
  const ShieldDecision d2 = shield.project(p1, Vec2{-0.30, -0.20});
  CHECK((d2.reason_mask & kShieldAccelerationLimit) != 0);
  const Vec2 delta1 = d1.safe_target - p0;
  const Vec2 delta2 = d2.safe_target - p1;
  CHECK((delta2 - delta1).norm() <= 0.02 + 1e-9);
}

TEST(shield_circle_cbf_zero_intrusion_and_tangential_slide) {
  SafetyEnvelope env = SafetyEnvelope::medical_surgical_fixture();
  SafetyShield shield(env);
  PushSim2D sim(42, SimConfig{});
  ShieldedRobotAdapter safe_robot(sim, shield, true);

  // Command straight through the center of Circle 0 (0.045, -0.185) toward (0.09, -0.07)
  const Vec2 goal{0.09, -0.07};
  for (int t = 0; t < 80; ++t) {
    safe_robot.step(goal);
  }
  // Zero intrusions AND smooth tangential slide around the virtual fixture to reach the target!
  CHECK(safe_robot.zone_intrusions() == 0);
  CHECK(safe_robot.min_true_clearance_m() >= 0.0);
  CHECK(shield.stats().circle_projections > 0);
  CHECK((safe_robot.robot_pose() - goal).norm() < 0.02);
}

TEST(shield_polygon_cbf_zero_intrusion_and_tangential_slide) {
  SafetyEnvelope env = SafetyEnvelope::medical_surgical_fixture();
  SafetyShield shield(env);
  PushSim2D sim(19, SimConfig{});
  sim.move_robot_to(Vec2{0.18, 0.14}, 80);   // place near goal A above-right of the polygon fixture

  ShieldedRobotAdapter safe_robot(sim, shield, true);
  // Command straight across Polygon 0 ((-0.02..0.07, 0.04..0.10)) toward lineup B (-0.10, -0.04)
  const Vec2 target{-0.10, -0.04};
  for (int t = 0; t < 100; ++t) {
    safe_robot.step(target);
  }
  CHECK(safe_robot.zone_intrusions() == 0);
  CHECK(safe_robot.min_true_clearance_m() >= 0.0);
  CHECK(shield.stats().polygon_projections > 0);
  CHECK((safe_robot.robot_pose() - target).norm() < 0.025);
}

TEST(shield_adversarial_random_rays_never_penetrate) {
  SafetyEnvelope env = SafetyEnvelope::medical_surgical_fixture();
  SafetyShield shield(env);
  PushSim2D sim(777, SimConfig{});
  ShieldedRobotAdapter safe_robot(sim, shield, true);
  Rng rng(0x5AFEC0DEULL);

  for (int t = 0; t < 2000; ++t) {
    // Adversarial targets including direct hits inside Circle 0, inside Polygon 0, and outside table bounds
    Vec2 adv;
    if (t % 4 == 0) adv = env.circles[0].center;
    else if (t % 4 == 1) adv = Vec2{0.040, 0.075};  // interior of polygon 0
    else adv = Vec2{rng.uniform(-0.8, 0.8), rng.uniform(-0.8, 0.8)};
    safe_robot.step(adv);
  }
  CHECK(safe_robot.zone_intrusions() == 0);
  CHECK(safe_robot.min_true_clearance_m() >= -1e-7);
  CHECK(shield.stats().interventions > 500);
}

TEST(shield_medical_cell_eliminates_unshielded_intrusions) {
  SafetyEnvelope env = SafetyEnvelope::medical_surgical_fixture();
  int unshielded_intrusions = 0;
  int shielded_intrusions = 0;
  int shielded_success = 0;
  CorrelationAwareRetry lib;

  for (int ep = 1000; ep < 1020; ++ep) {
    SimConfig sc; sc.field_faults = true;
    // 1) Unshielded NaiveRetry (audit-only ShieldedRobotAdapter with enforce=false)
    {
      PushSim2D sim(ep, sc);
      SafetyShield sh(env);
      ShieldedRobotAdapter audit_robot(sim, sh, false);
      ScriptedPushPolicy pol(0.15);
      pol.new_episode(ep);
      NaiveRetry naive;
      MiddlewareConfig mc; mc.max_retries = 3; mc.seed = ep;
      ReliabilityMiddleware mw(pol, audit_robot, &naive, mc);
      auto task = make_task(sim, ep);
      mw.run(task);
      if (audit_robot.zone_intrusions() > 0) ++unshielded_intrusions;
    }
    // 2) Shielded relmw (enforce=true + CorrelationAwareRetry)
    {
      PushSim2D sim(ep, sc);
      SafetyShield sh(env);
      ShieldedRobotAdapter safe_robot(sim, sh, true);
      ScriptedPushPolicy pol(0.15);
      pol.new_episode(ep);
      MiddlewareConfig mc; mc.max_retries = 3; mc.seed = ep;
      ReliabilityMiddleware mw(pol, safe_robot, &lib, mc);
      auto task = make_task(sim, ep);
      mw.run(task);
      if (safe_robot.zone_intrusions() > 0) ++shielded_intrusions;
      Obs ts = sim.true_state();
      if (oracle_verdict(ts, 0).ok && oracle_verdict(ts, 1).ok) ++shielded_success;
    }
  }
  CHECK(unshielded_intrusions >= 15);   // unshielded trajectory regularly violates surgical No-Go Zones
  CHECK(shielded_intrusions == 0);      // 100.0% No-Go Zone Zero-Intrusion Rate!
  CHECK(shielded_success >= 17);        // high task completion maintained while sliding around fixtures
}

TEST_MAIN()
