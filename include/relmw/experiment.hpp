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
#include "relmw/nn_policy.hpp"
#include "relmw/safety_shield.hpp"
#include "relmw/sim2d.hpp"
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace relmw {

enum class Arm { SinglePass, NaiveRetry, CorrAware };
const char* to_string(Arm);

enum class PolicyKind : std::uint8_t { Scripted = 0, TinyMLP_FP32 = 1, TinyMLP_Q12 = 2 };
const char* to_string(PolicyKind);

enum class CellPreset : std::uint8_t { Standard = 0, MedicalVial = 1 };
const char* to_string(CellPreset);

struct EpisodeResult {
  int ep = 0;
  std::string condition;
  Arm arm = Arm::SinglePass;
  Fault fault = Fault::None;
  bool gt_success = false, mw_success = false;
  std::string abort_reason, failed_at;
  long sim_steps = 0;
  std::uint64_t zone_intrusions = 0;
  std::uint64_t shield_interventions = 0;
  double min_clearance_m = 1.0;
  std::vector<Attempt> attempts;
};

struct ExperimentConfig {
  int n = 100;
  int warmup = 40;
  int eval_seed0 = 1000, warmup_seed0 = 5000;
  double policy_bias_std = 0.15;
  PolicyKind policy_kind = PolicyKind::Scripted;
  CellPreset preset = CellPreset::Standard;
  bool enable_shield = false;   // when true, CorrAware arm enforces 1 kHz Simplex SafetyShield
};

std::vector<Subtask> make_task(TruthSensor& truth, std::uint64_t ep);
EpisodeResult run_episode(int ep, bool field, Arm arm, RetryPolicy* rp, const ExperimentConfig& cfg,
                          EventSink sink = nullptr);

// Plan control (F2): parse a comma-separated plan CSV ("push_B,push_A") into items. Rules: non-empty items, no
// spaces, names [A-Za-z0-9_]+, at most 8 items. On error `out` is cleared, false is returned and `err` explains.
bool parse_plan(const std::string& csv, std::vector<std::string>& out, std::string* err);

// Reorder/filter `task` in place to the plan order. A valid plan is non-empty, has no duplicates and names only
// subtasks present in `task`. On any error `task` is left unchanged, false is returned and `err` explains.
bool apply_plan(std::vector<Subtask>& task, const std::vector<std::string>& plan, std::string* err);

// Full benchmark (warm-up + nominal/field x 3 arms). Writes summary JSON + per-episode JSONL.
struct BenchOutput { std::string summary_json; std::string episodes_jsonl; std::string library_txt; std::string table; };
BenchOutput run_benchmark(const ExperimentConfig& cfg);

// Record a real 128-byte binary TelemetryFrame showcase comparing NaiveRetry (unshielded MLP hitting surgical No-Go
// Zone) vs relmw-qnx (1 kHz Simplex SafetyShield tangential slide + relocalize + mid-episode relmw_robotd crash/reconnect).
bool export_replay_showcase(const std::string& bin_path);

}  // namespace relmw
