// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Unit tests for the deterministic 2-D pushing simulator.
#include "relmw/sim2d.hpp"
#include "relmw/policy.hpp"
#include "minitest.hpp"
#include <cmath>
#include <vector>

using namespace relmw;

TEST(sim_nominal_has_no_fault) {
  for (unsigned s = 0; s < 200; ++s) {
    PushSim2D sim(s, SimConfig{});
    CHECK(sim.fault() == Fault::None);
  }
}

TEST(sim_field_fault_frequencies_match_weights) {
  SimConfig cfg;
  cfg.field_faults = true;
  int cnt[4] = {0, 0, 0, 0};
  const int N = 1000;
  for (unsigned s = 0; s < N; ++s) {
    PushSim2D sim(s, cfg);
    ++cnt[static_cast<int>(sim.fault())];
  }
  const double w[4] = {cfg.w_none, cfg.w_bump, cfg.w_percep, cfg.w_slip};
  for (int f = 0; f < 4; ++f) {
    double freq = static_cast<double>(cnt[f]) / N;
    CHECK(std::fabs(freq - w[f]) <= 0.06);
  }
}

TEST(sim_same_seed_deterministic_traj) {
  SimConfig cfg;
  cfg.field_faults = true;
  PushSim2D a(123, cfg), b(123, cfg);
  std::vector<Vec2> targets;
  for (int t = 0; t < 300; ++t)
    targets.push_back(Vec2{0.30 * std::sin(0.11 * t), -0.10 + 0.25 * std::cos(0.07 * t)});
  for (int t = 0; t < 300; ++t) {
    a.step(targets[t]);
    b.step(targets[t]);
    Obs ta = a.true_state(), tb = b.true_state();
    CHECK(ta.pusher.x == tb.pusher.x && ta.pusher.y == tb.pusher.y);
    for (int i = 0; i < kNumObjects; ++i)
      CHECK(ta.obj[i].x == tb.obj[i].x && ta.obj[i].y == tb.obj[i].y);
  }
}

TEST(sim_expert_pushes_cube_a_to_goal) {
  PushSim2D sim(7, SimConfig{});   // nominal
  for (int t = 0; t < 200; ++t) {
    Obs truth = sim.true_state();
    Vec2 tgt = expert_action(truth, 0, goal_of(0), 0.0);
    sim.step(tgt);
  }
  double d = (sim.true_state().obj[0] - goal_of(0)).norm();
  CHECK(d < kGoalTol);
}

TEST(sim_percep_bias_offset_bounds_and_uniformity) {
  SimConfig cfg;
  cfg.field_faults = true;
  cfg.force_fault = 2;               // PercepBias
  PushSim2D sim(11, cfg);
  Obs o = sim.get_obs(), truth = sim.true_state();
  Vec2 bias0 = o.obj[0] - truth.obj[0];
  Vec2 bias1 = o.obj[1] - truth.obj[1];
  double n0 = bias0.norm(), n1 = bias1.norm();
  CHECK(n0 >= 0.04 && n0 <= 0.07);
  CHECK(n1 >= 0.04 && n1 <= 0.07);
  CHECK(bias0.x == bias1.x && bias0.y == bias1.y);   // same offset vector for both objects
}

TEST(sim_no_fault_obs_is_truth) {
  PushSim2D sim(13, SimConfig{});
  Obs o = sim.get_obs(), truth = sim.true_state();
  CHECK((o.obj[0] - truth.obj[0]).norm() == 0.0);
  CHECK((o.obj[1] - truth.obj[1]).norm() == 0.0);
}

TEST(sim_knock_moves_object_by_exact_delta) {
  PushSim2D sim(3, SimConfig{});
  Vec2 before = sim.true_state().obj[0];
  Vec2 delta{0.05, -0.03};
  sim.knock(0, delta);
  Vec2 after = sim.true_state().obj[0];
  CHECK(after.x == before.x + delta.x && after.y == before.y + delta.y);
}

TEST(sim_move_robot_to_reaches_free_target) {
  PushSim2D sim(5, SimConfig{});
  Vec2 target{0.0, -0.35};           // far from both cubes' start region
  sim.move_robot_to(target);
  CHECK((sim.robot_pose() - target).norm() < 0.01);
}

TEST(sim_slippery_friction_reduced) {
  SimConfig cfg;
  cfg.field_faults = true;
  cfg.force_fault = 3;               // Slippery
  PushSim2D sim(17, cfg);
  CHECK(sim.fault() == Fault::Slippery);
  CHECK(sim.friction() < 0.15 * 0.6 * 1.15 + 1e-12);
}

TEST_MAIN()
