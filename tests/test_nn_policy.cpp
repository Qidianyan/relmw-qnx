// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Unit & paired S+ benchmark tests for the real trained Tiny-MLP policy (nn_policy.hpp) + 1 kHz Simplex SafetyShield.
#include "minitest.hpp"
#include "relmw/experiment.hpp"
#include "relmw/nn_policy.hpp"
#include "relmw/retry_policy.hpp"
#include "relmw/safety_shield.hpp"
#include "relmw/stats.hpp"
#include "relmw/telemetry_bridge.hpp"
#include "relmw/verifier.hpp"
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <vector>

using namespace relmw;

TEST(nn_policy_fp32_and_q12_parity) {
  Rng rng(2026);
  double max_diff = 0.0;
  for (int k = 0; k < 500; ++k) {
    Obs obs;
    obs.pusher = Vec2{rng.uniform(-0.35, 0.35), rng.uniform(-0.32, 0.25)};
    obs.obj[0] = Vec2{rng.uniform(-0.28, 0.28), rng.uniform(-0.10, 0.22)};
    obs.obj[1] = Vec2{rng.uniform(-0.28, 0.28), rng.uniform(-0.10, 0.22)};
    float feat[TinyMLPPolicy::kInDim];
    float out_fp[TinyMLPPolicy::kOutDim];
    float out_q12[TinyMLPPolicy::kOutDim];
    TinyMLPPolicy::extract_features(obs, k % 2, goal_of(k % 2), feat);
    TinyMLPPolicy::forward_fp32(feat, out_fp);
    TinyMLPPolicy::forward_q12(feat, out_q12);
    for (int j = 0; j < TinyMLPPolicy::kOutDim; ++j) {
      CHECK(std::isfinite(out_fp[j]));
      CHECK(std::isfinite(out_q12[j]));
      max_diff = std::max(max_diff, std::fabs(static_cast<double>(out_fp[j] - out_q12[j])));
    }
  }
  CHECK(max_diff < 0.004);   // Q12 fixed-point matches IEEE float32 within < 4 mm across random poses
}

TEST(nn_policy_nominal_unseen_seeds_high_accuracy) {
  ExperimentConfig cfg;
  cfg.policy_kind = PolicyKind::TinyMLP_FP32;
  cfg.policy_bias_std = 0.08;
  int ok_fp = 0, ok_q12 = 0;
  for (int ep = 2000; ep < 2040; ++ep) {
    cfg.policy_kind = PolicyKind::TinyMLP_FP32;
    if (run_episode(ep, false, Arm::SinglePass, nullptr, cfg).gt_success) ++ok_fp;
    cfg.policy_kind = PolicyKind::TinyMLP_Q12;
    if (run_episode(ep, false, Arm::SinglePass, nullptr, cfg).gt_success) ++ok_q12;
  }
  CHECK(ok_fp >= 38);    // >= 95% nominal accuracy on unseen seeds
  CHECK(ok_q12 >= 38);   // >= 95% nominal accuracy for FPU-free Q12 quantized weights
}

TEST(nn_policy_splus_medical_shield_benchmark_n100) {
  ExperimentConfig cfg;
  cfg.n = 100;
  cfg.warmup = 40;
  cfg.policy_bias_std = 0.12;
  cfg.policy_kind = PolicyKind::TinyMLP_FP32;
  cfg.preset = CellPreset::MedicalVial;
  cfg.enable_shield = true;

  // Run the full S+ paired benchmark (40 held-out calibration warmup seeds + 100 frozen evaluation seeds x 3 arms)
  const BenchOutput bench = run_benchmark(cfg);
  CHECK(!bench.summary_json.empty());

  // Re-verify exact per-episode counts from the frozen failure library produced by run_benchmark
  CorrelationAwareRetry lib;
  {
    char lpath[] = "/tmp/relmw_nnlib_XXXXXX";
    const int fd = mkstemp(lpath);
    CHECK(fd >= 0);
    if (fd >= 0) {
      const ssize_t wr = ::write(fd, bench.library_txt.data(), bench.library_txt.size());
      CHECK(wr == static_cast<ssize_t>(bench.library_txt.size()));
      ::close(fd);
      CHECK(lib.load(lpath));
      ::unlink(lpath);
    }
  }
  lib.learn = false;

  NaiveRetry naive;
  int ok_none = 0, ok_naive = 0, ok_mw = 0;
  int intr_none = 0, intr_naive = 0, intr_mw = 0;
  int fixed = 0, broken = 0;
  std::uint64_t mw_shield_pulses = 0;

  for (int ep = cfg.eval_seed0; ep < cfg.eval_seed0 + cfg.n; ++ep) {
    const EpisodeResult r0 = run_episode(ep, true, Arm::SinglePass, nullptr, cfg);
    const EpisodeResult r1 = run_episode(ep, true, Arm::NaiveRetry, &naive, cfg);
    const EpisodeResult r2 = run_episode(ep, true, Arm::CorrAware, &lib, cfg);
    ok_none += r0.gt_success ? 1 : 0;
    ok_naive += r1.gt_success ? 1 : 0;
    ok_mw += r2.gt_success ? 1 : 0;
    intr_none += r0.zone_intrusions > 0 ? 1 : 0;
    intr_naive += r1.zone_intrusions > 0 ? 1 : 0;
    intr_mw += r2.zone_intrusions > 0 ? 1 : 0;
    mw_shield_pulses += r2.shield_interventions;
    if (r2.gt_success && !r0.gt_success) ++fixed;
    if (r0.gt_success && !r2.gt_success) ++broken;
  }

  const double p_val = stats::mcnemar_exact(fixed, broken);
  std::printf("  [Tiny-MLP + MedicalVial Shield N=100] success: none=%d naive=%d relmw=%d (McNemar p=%.3g) | "
              "No-Go intrusions: none=%d/100 naive=%d/100 relmw=%d/100 (zero-intrusion=%.1f%%, shield_pulses=%llu)\n",
              ok_none, ok_naive, ok_mw, p_val, intr_none, intr_naive, intr_mw,
              100.0 * (100 - intr_mw) / 100.0, static_cast<unsigned long long>(mw_shield_pulses));

  CHECK(ok_mw >= 92);
  CHECK(ok_mw - ok_none >= 15);
  CHECK(p_val < 1e-3);
  CHECK(intr_none >= 50);          // unshielded MLP routinely crosses surgical No-Go Zones
  CHECK(intr_naive >= 50);
  CHECK(intr_mw == 0);             // 100.0% No-Go Zone Zero-Intrusion Rate!
  CHECK(mw_shield_pulses > 1000);  // 1 kHz Simplex Safety Shield actively projected hazardous commands
}

TEST(nn_policy_showcase_replay_binary_export) {
  char path[] = "/tmp/relmw_showcase_XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  if (fd >= 0) ::close(fd);

  CHECK(export_replay_showcase(path));
  std::vector<TelemetryFrame> frames;
  CHECK(TelemetryRing::load_binary(path, frames));
  CHECK(frames.size() >= 60);

  int arm1 = 0, arm2 = 0, arm1_intr = 0, arm2_intr = 0, arm2_shield = 0, link_lost = 0, link_rec = 0;
  for (const auto& f : frames) {
    if (f.arm_id == 1) {
      ++arm1;
      if (f.flags & kTelNoGoIntrusion) ++arm1_intr;
    } else if (f.arm_id == 2) {
      ++arm2;
      if (f.flags & kTelNoGoIntrusion) ++arm2_intr;
      if (f.flags & kTelShieldIntervened) ++arm2_shield;
      if (f.flags & kTelRobotLinkLost) ++link_lost;
      if (f.flags & kTelRobotReconnected) ++link_rec;
    }
  }
  CHECK(arm1 > 20 && arm2 > 20);
  CHECK(arm1_intr > 0);      // NaiveRetry hits surgical No-Go Zone
  CHECK(arm2_intr == 0);     // relmw-qnx has 0 intrusions
  CHECK(arm2_shield > 0);    // 1 kHz Simplex SafetyShield projected trajectory
  CHECK(link_lost == 1 && link_rec == 1);  // mid-episode kill -9 relmw_robotd + reconnect captured!
  ::unlink(path);
}

TEST_MAIN()
