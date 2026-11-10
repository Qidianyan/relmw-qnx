// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Subtask verifiers. The middleware only sees Verdicts; ground truth is never used for decisions.
#pragma once
#include "relmw/sim2d.hpp"

namespace relmw {

// Safety margin: the camera verifier only accepts a placement this far inside the goal tolerance
// (same 0.5 cm margin as the Python vision verifier), so sensor noise cannot pass a borderline result.
constexpr double kVerifierMargin = 0.005;

// Failure taxonomy from an object estimate + robot proprioception (same rules as the Python demo).
FailureMode classify(Vec2 obj, Vec2 pusher, Vec2 goal);

// Independent "overhead camera" verifier: its own object estimate (true pose + N(0, sigma) noise),
// unaffected by the policy's perception faults. Noise stream is seeded per episode for determinism.
class CameraVerifier {
 public:
  CameraVerifier(const PushSim2D* sim, int obj, std::uint64_t seed, double sigma = 0.003)
      : sim_(sim), obj_(obj), rng_(seed), sigma_(sigma) {}
  Verdict operator()(const Obs&);
 private:
  const PushSim2D* sim_;
  int obj_;
  Rng rng_;
  double sigma_;
};

// Ground-truth predicate (EVALUATION ONLY: scoring + verifier confusion matrix).
Verdict oracle_verdict(const Obs& truth, int obj);

}  // namespace relmw
