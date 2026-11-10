// The reliability middleware state machine (C++ port of reliability_mw/core.py).
//
//   for each subtask:  CHECKPOINT -> RUN(policy, verifier every k ticks, settle/hold) -> VERIFY
//        ok  -> next subtask
//        fail-> classify -> fingerprint (correlated?) -> library update -> ROLLBACK (robot only)
//               -> choose strategy (naive: replay | corr-aware: library posterior) -> RETRY
//               -> budget exhausted / ladder exhausted -> ESCALATE to operator
//   FINAL CHECK: re-verify every postcondition (later subtasks can disturb earlier ones); redo once.
#pragma once
#include "relmw/retry_policy.hpp"
#include <functional>
#include <string>
#include <vector>

namespace relmw {

struct Subtask {
  std::string name;
  VerifierFn verifier;
  int max_steps = 200;
  int check_every = 5;          // run the verifier every k ticks
  int hold = 2;                 // consecutive positive checks required (object must have settled)
  VerifierFn oracle;            // EVALUATION ONLY, may be empty
  SubtaskMeta meta;
};

enum class EventType { SubtaskStart, VerifyFail, Rollback, Retry, Escalate, SubtaskOk, EpisodeDone };
const char* to_string(EventType);

struct Event {
  EventType type;
  std::string subtask;
  int attempt = 0;
  std::string strategy;
  FailureMode mode = FailureMode::None;
  bool correlated = false;
  long steps_used = 0;
};
using EventSink = std::function<void(const Event&)>;

struct EpisodeReport {
  bool success = false;
  std::vector<Attempt> attempts;
  std::string failed_at, abort_reason;
  int n_failures() const;
  bool recovered() const { return success && n_failures() > 0; }
};

struct MiddlewareConfig {
  int max_retries = 3;          // 0 => middleware disabled ("single pass")
  long episode_step_budget = 2000;
  bool final_check = true;
  std::uint64_t seed = 0;
};

class ReliabilityMiddleware {
 public:
  ReliabilityMiddleware(Policy& policy, RobotAdapter& robot, RetryPolicy* retry, const MiddlewareConfig& cfg,
                        EventSink sink = nullptr);
  EpisodeReport run(std::vector<Subtask>& subtasks);

 private:
  struct SubResult { bool ok; FailureMode mode; int steps; Verdict info; Obs obs; };
  SubResult run_subtask(Subtask& st, const Perturb& p, Strategy* strategy);
  bool run_with_retries(Subtask& st, EpisodeReport& rep, const std::string& tag);
  void emit(Event e);

  Policy& policy_;
  RobotAdapter& robot_;
  RetryPolicy* retry_;
  NaiveRetry naive_;
  MiddlewareConfig cfg_;
  Rng rng_, strat_rng_;
  EventSink sink_;
  long used_ = 0;
};

}  // namespace relmw
