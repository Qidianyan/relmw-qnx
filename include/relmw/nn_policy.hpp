// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Zero-dependency, allocation-free C++17 two-hidden-layer MLP policy (TinyMLPPolicy).
//
// Architecture: 8 -> 32 (ReLU) -> 32 (ReLU) -> 2 (task-frame delta target).
// Weights are trained offline by tools/train_nn_policy.py (Behavioural Cloning + DAgger rollouts on the exact 2-D
// contact dynamics, including peripheral camera occlusion vulnerability) and exported as static constexpr tables in
// relmw/nn_policy_weights.hpp in BOTH IEEE-754 float32 and quantized int16 Q12 fixed-point formats.
//
// Forward inference uses fixed stack arrays only: ZERO heap allocations, < 2 us per tick on x86_64/ARM64.
#pragma once
#include "relmw/robot.hpp"
#include <cstdint>

namespace relmw {

enum class MLPInferenceMode : std::uint8_t {
  Float32 = 0,    // constexpr float weights + FMA
  FixedQ12 = 1,   // constexpr int16_t Q12 weights + int32_t accumulator (FPU-free embedded mode)
};

class TinyMLPPolicy : public Policy {
 public:
  static constexpr int kInDim = 8;
  static constexpr int kH1Dim = 32;
  static constexpr int kH2Dim = 32;
  static constexpr int kOutDim = 2;
  static constexpr int kParamCount = kInDim * kH1Dim + kH1Dim + kH1Dim * kH2Dim + kH2Dim + kH2Dim * kOutDim + kOutDim;

  explicit TinyMLPPolicy(MLPInferenceMode mode = MLPInferenceMode::Float32,
                         double ep_jitter_std = 0.10,
                         std::uint64_t base_seed = 0) noexcept
      : mode_(mode), ep_jitter_std_(ep_jitter_std), base_seed_(base_seed) {}

  void new_episode(std::uint64_t ep) noexcept { ep_ = ep; }
  MLPInferenceMode mode() const noexcept { return mode_; }
  void set_mode(MLPInferenceMode m) noexcept { mode_ = m; }

  // Allocation-free forward pass implementing Policy::act().
  Vec2 act(const Obs& obs, const std::string& subtask, const SubtaskMeta& meta, const Perturb& p) override;

  // Raw allocation-free neural network evaluation (exposed for unit tests & Q12 vs FP32 parity verification).
  static void extract_features(const Obs& obs, int obj_idx, Vec2 goal, float out_feat[kInDim]) noexcept;
  static void forward_fp32(const float in[kInDim], float out[kOutDim]) noexcept;
  static void forward_q12(const float in[kInDim], float out[kOutDim]) noexcept;

 private:
  MLPInferenceMode mode_;
  double ep_jitter_std_;
  std::uint64_t base_seed_;
  std::uint64_t ep_ = 0;
};

}  // namespace relmw
