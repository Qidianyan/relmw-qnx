// Retry policies: NaiveRetry (rollback + replay) and CorrelationAwareRetry (failure library).
#pragma once
#include "relmw/strategies.hpp"
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace relmw {

struct Attempt {
  std::string subtask;
  int attempt = 0;              // -1 = final postcondition re-check
  bool ok = false;              // verifier verdict (what the middleware believed)
  FailureMode mode = FailureMode::None;
  int steps = 0;
  std::string strategy = "first_try";
  bool correlated = false;      // repeated the previous failure's fingerprint
  bool predicted_correlated = false;
  int gt_ok = -1;               // oracle verdict at attempt end (-1 = n/a); evaluation only
  bool has_end_obj = false;
  Vec2 end_obj;
};

constexpr double kRepeatPosTol = 0.02;   // m: end-pose match for "same failure again"
bool is_repeat(const Attempt& a, const std::vector<Attempt>& history);

struct Choice { std::unique_ptr<Strategy> strategy; bool predicted_correlated = false; };

class RetryPolicy {
 public:
  virtual ~RetryPolicy() = default;
  virtual const char* name() const = 0;
  virtual void new_episode() {}
  // Strategy for retry k>=1; nullptr strategy => escalate to a human operator.
  virtual Choice choose(const std::string& subtask, const std::vector<Attempt>& history, int k) = 0;
  // Optional strategy applied pro-actively on the first attempt (shared-cause fix).
  virtual std::unique_ptr<Strategy> proactive(const std::string&) { return nullptr; }
  virtual void record(const std::string& subtask, FailureMode mode, const std::string& strategy, bool ok) {
    (void)subtask; (void)mode; (void)strategy; (void)ok;
  }
};

class NaiveRetry : public RetryPolicy {
 public:
  const char* name() const override { return "naive_retry"; }
  Choice choose(const std::string&, const std::vector<Attempt>&, int) override;
};

// Failure-library-driven escalation. Score = posterior mean of P(success | subtask, mode, strategy)
// with an engineer's prior table as pseudo-counts. Never repeats a strategy that already failed in this
// subtask; escalates to a human when the ladder is exhausted. Persistent fixes (relocalize, slow_mode)
// that worked once in an episode are applied pro-actively to later subtasks (shared cause).
class CorrelationAwareRetry : public RetryPolicy {
 public:
  const char* name() const override { return "corr_aware_retry"; }
  void new_episode() override { active_.clear(); }
  Choice choose(const std::string& subtask, const std::vector<Attempt>& history, int k) override;
  std::unique_ptr<Strategy> proactive(const std::string&) override;
  void record(const std::string& subtask, FailureMode mode, const std::string& strategy, bool ok) override;
  double score(const std::string& subtask, FailureMode mode, const std::string& strat) const;

  bool learn = true;
  // Library: "subtask|mode|strategy" -> (successes, trials). Text format: one "key s n" per line.
  std::map<std::string, std::pair<int, int>> lib;
  bool save(const std::string& path) const;
  bool load(const std::string& path);
 private:
  std::vector<std::string> active_;
};

}  // namespace relmw
