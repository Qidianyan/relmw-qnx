// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
#include "relmw/nn_policy.hpp"
#include "relmw/nn_policy_weights.hpp"
#include "relmw/rng.hpp"
#include <algorithm>
#include <cmath>

namespace relmw {

namespace {

// Portable arithmetic floor shift right by 12 bits (matches two's-complement >> 12 without C++17 signed-shift UB).
inline std::int32_t asr12(std::int32_t x) noexcept {
  if (x >= 0) return x >> nn_weights::kQ12Shift;
  const std::uint32_t ux = static_cast<std::uint32_t>(-static_cast<std::int64_t>(x));
  const std::uint32_t q = (ux + ((1u << nn_weights::kQ12Shift) - 1u)) >> nn_weights::kQ12Shift;
  return -static_cast<std::int32_t>(q);
}

}  // namespace

void TinyMLPPolicy::extract_features(const Obs& obs, int obj_idx, Vec2 goal, float out_feat[kInDim]) noexcept {
  const Vec2 b = obs.obj[obj_idx];
  const Vec2 dbg = goal - b;
  const double l_bg = dbg.norm();
  const Vec2 u = dbg * (1.0 / (l_bg + 1e-9));
  const Vec2 v{-u.y, u.x};
  const Vec2 r_pb = obs.pusher - b;
  const double along = r_pb.dot(u);
  const double lat = r_pb.dot(v);
  const double dist_pb = r_pb.norm();

  out_feat[0] = static_cast<float>(std::clamp(along / 0.25, -2.0, 2.0));
  out_feat[1] = static_cast<float>(std::clamp(lat / 0.15, -2.0, 2.0));
  out_feat[2] = static_cast<float>(std::clamp(dist_pb / 0.35, 0.0, 2.5));
  out_feat[3] = static_cast<float>(std::clamp(l_bg / 0.40, 0.0, 2.5));
  out_feat[4] = static_cast<float>(u.x);
  out_feat[5] = static_cast<float>(u.y);
  out_feat[6] = static_cast<float>(std::clamp(b.x / 0.35, -1.5, 1.5));
  out_feat[7] = static_cast<float>(std::clamp(b.y / 0.30, -1.5, 1.5));
}

void TinyMLPPolicy::forward_fp32(const float in[kInDim], float out[kOutDim]) noexcept {
  float h1[kH1Dim];
  for (int j = 0; j < kH1Dim; ++j) {
    float acc = nn_weights::kB1[j];
    for (int i = 0; i < kInDim; ++i) {
      acc += in[i] * nn_weights::kW1[i * kH1Dim + j];
    }
    h1[j] = acc > 0.0f ? acc : 0.0f;
  }

  float h2[kH2Dim];
  for (int j = 0; j < kH2Dim; ++j) {
    float acc = nn_weights::kB2[j];
    for (int i = 0; i < kH1Dim; ++i) {
      acc += h1[i] * nn_weights::kW2[i * kH2Dim + j];
    }
    h2[j] = acc > 0.0f ? acc : 0.0f;
  }

  for (int j = 0; j < kOutDim; ++j) {
    float acc = nn_weights::kB3[j];
    for (int i = 0; i < kH2Dim; ++i) {
      acc += h2[i] * nn_weights::kW3[i * kOutDim + j];
    }
    out[j] = acc;
  }
}

void TinyMLPPolicy::forward_q12(const float in[kInDim], float out[kOutDim]) noexcept {
  std::int32_t x_q[kInDim];
  for (int i = 0; i < kInDim; ++i) {
    const float scaled = std::clamp(in[i], -7.5f, 7.5f) * nn_weights::kQ12Scale;
    x_q[i] = static_cast<std::int32_t>(std::lrintf(scaled));
  }

  std::int32_t h1_q[kH1Dim];
  for (int j = 0; j < kH1Dim; ++j) {
    std::int32_t acc = 0;
    for (int i = 0; i < kInDim; ++i) {
      acc += x_q[i] * static_cast<std::int32_t>(nn_weights::kW1_q12[i * kH1Dim + j]);
    }
    const std::int32_t z = asr12(acc) + static_cast<std::int32_t>(nn_weights::kB1_q12[j]);
    h1_q[j] = z > 0 ? z : 0;
  }

  std::int32_t h2_q[kH2Dim];
  for (int j = 0; j < kH2Dim; ++j) {
    std::int32_t acc = 0;
    for (int i = 0; i < kH1Dim; ++i) {
      acc += h1_q[i] * static_cast<std::int32_t>(nn_weights::kW2_q12[i * kH2Dim + j]);
    }
    const std::int32_t z = asr12(acc) + static_cast<std::int32_t>(nn_weights::kB2_q12[j]);
    h2_q[j] = z > 0 ? z : 0;
  }

  for (int j = 0; j < kOutDim; ++j) {
    std::int32_t acc = 0;
    for (int i = 0; i < kH2Dim; ++i) {
      acc += h2_q[i] * static_cast<std::int32_t>(nn_weights::kW3_q12[i * kOutDim + j]);
    }
    const std::int32_t z = asr12(acc) + static_cast<std::int32_t>(nn_weights::kB3_q12[j]);
    out[j] = static_cast<float>(z) / nn_weights::kQ12Scale;
  }
}

Vec2 TinyMLPPolicy::act(const Obs& obs, const std::string& subtask, const SubtaskMeta& meta, const Perturb& p) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char ch : subtask) {
    h ^= ch;
    h *= 1099511628211ULL;
  }
  Rng r(hash_mix({base_seed_, ep_, h, p.has_seed ? p.seed + 1 : 0}));
  const double bias = ep_jitter_std_ > 0.0 ? r.normal(0.0, ep_jitter_std_) : 0.0;

  const Vec2 b = obs.obj[meta.object];
  const Vec2 to_goal = meta.goal_xy - b;
  const Vec2 u = to_goal.unit();
  const Vec2 v{-u.y, u.x};

  float feat[kInDim];
  float raw_out[kOutDim];
  extract_features(obs, meta.object, meta.goal_xy, feat);
  if (mode_ == MLPInferenceMode::FixedQ12) {
    forward_q12(feat, raw_out);
  } else {
    forward_fp32(feat, raw_out);
  }

  double da = std::clamp(static_cast<double>(raw_out[0]), -0.15, 0.15);
  double dl = std::clamp(static_cast<double>(raw_out[1]), -0.15, 0.15);
  // Apply unimodal policy heading bias during close-in pushing (mirrors learned policy contact variance without
  // corrupting the goal coordinate itself).
  if ((obs.pusher - b).norm() < 0.085 && bias != 0.0) {
    const double cb = std::cos(bias), sb = std::sin(bias);
    const double r_da = cb * da - sb * dl;
    const double r_dl = sb * da + cb * dl;
    da = r_da;
    dl = r_dl;
  }
  return obs.pusher + u * da + v * dl;
}

}  // namespace relmw
