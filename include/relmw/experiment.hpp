// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Paired benchmark: same policy, same episode seeds, same verifier; three arms.
//   single_pass      : no retries (verifier only sequences subtasks)   = "no middleware"
//   naive_retry      : rollback + replay, up to 3 retries per subtask
//   corr_aware_retry : failure-library strategy choice + shared-cause fixes + final check.
//                      Library warmed on held-out seeds (5000..), then FROZEN for evaluation.
// Ground truth is used ONLY for scoring, never for decisions.
#pragma once
#include "relmw/middleware.hpp"
#include "relmw/sim2d.hpp"
#include <ostream>
#include <string>
#include <vector>

namespace relmw {

enum class Arm { SinglePass, NaiveRetry, CorrAware };
const char* to_string(Arm);

struct EpisodeResult {
  int ep = 0;
  std::string condition;
  Arm arm = Arm::SinglePass;
  Fault fault = Fault::None;
  bool gt_success = false, mw_success = false;
  std::string abort_reason, failed_at;
  long sim_steps = 0;
  std::vector<Attempt> attempts;
};

struct ExperimentConfig {
  int n = 100;
  int warmup = 40;
  int eval_seed0 = 1000, warmup_seed0 = 5000;
  double policy_bias_std = 0.15;
};

std::vector<Subtask> make_task(PushSim2D& sim, std::uint64_t ep);
EpisodeResult run_episode(int ep, bool field, Arm arm, RetryPolicy* rp, const ExperimentConfig& cfg,
                          EventSink sink = nullptr);

// Full benchmark (warm-up + nominal/field x 3 arms). Writes summary JSON + per-episode JSONL.
struct BenchOutput { std::string summary_json; std::string episodes_jsonl; std::string library_txt; std::string table; };
BenchOutput run_benchmark(const ExperimentConfig& cfg);

}  // namespace relmw
