// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Scripted pushing policy standing in for a learned policy (ACT / diffusion / VLA).
#pragma once
#include "relmw/robot.hpp"
#include <cstdint>

namespace relmw {

// Smooth, unimodal pushing expert. Each attempt gets a heading bias drawn from
// hash(episode, subtask, perturb seed): the FIRST attempt is the policy's nominal behaviour
// (deterministic per episode); a retry with a fresh perturb seed gets an independent bias.
class ScriptedPushPolicy : public Policy {
 public:
  explicit ScriptedPushPolicy(double bias_std = 0.15, std::uint64_t base_seed = 0)
      : bias_std_(bias_std), base_seed_(base_seed) {}
  void new_episode(std::uint64_t ep) { ep_ = ep; }
  Vec2 act(const Obs& obs, const std::string& subtask, const SubtaskMeta& meta, const Perturb& p) override;
 private:
  double bias_std_;
  std::uint64_t base_seed_, ep_ = 0;
};

// Pure expert action (also used by tests). `bias` rotates the push heading (rad).
Vec2 expert_action(const Obs& obs, int obj, Vec2 goal, double bias);

}  // namespace relmw
