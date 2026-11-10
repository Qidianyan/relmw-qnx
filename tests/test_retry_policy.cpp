// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Unit tests for the retry policies (naive replay + correlation-aware failure library).
#include "relmw/retry_policy.hpp"
#include "relmw/strategies.hpp"
#include "minitest.hpp"
#include <string>
#include <unistd.h>

using namespace relmw;

static Attempt fail(const std::string& sub, FailureMode m, const std::string& strat, Vec2 end) {
  Attempt a;
  a.subtask = sub;
  a.ok = false;
  a.mode = m;
  a.strategy = strat;
  a.has_end_obj = true;
  a.end_obj = end;
  return a;
}

TEST(retry_is_repeat_same_end_pose) {
  Attempt a = fail("push_A", FailureMode::StoppedShort, "replay", {0.10, 0.10});
  std::vector<Attempt> h = {a};
  CHECK(is_repeat(a, h));
}

TEST(retry_is_repeat_far_end_pose) {
  Attempt a = fail("push_A", FailureMode::StoppedShort, "replay", {0.10, 0.10});
  std::vector<Attempt> h = {fail("push_A", FailureMode::StoppedShort, "replay", {0.15, 0.10})};
  CHECK(!is_repeat(a, h));   // 0.05 m apart > kRepeatPosTol
}

TEST(retry_is_repeat_different_mode) {
  Attempt a = fail("push_A", FailureMode::StoppedShort, "replay", {0.10, 0.10});
  std::vector<Attempt> h = {fail("push_A", FailureMode::OffAxisDrift, "replay", {0.10, 0.10})};
  CHECK(!is_repeat(a, h));
}

TEST(retry_is_repeat_skips_ok_attempts) {
  Attempt a = fail("push_A", FailureMode::StoppedShort, "replay", {0.10, 0.10});
  Attempt ok = a;
  ok.ok = true;
  ok.mode = FailureMode::None;
  std::vector<Attempt> h = {ok, ok, fail("push_A", FailureMode::StoppedShort, "replay", {0.105, 0.10})};
  CHECK(is_repeat(a, h));            // ok attempts are skipped, later failure matches
  std::vector<Attempt> only_ok = {ok, ok};
  CHECK(!is_repeat(a, only_ok));     // ok-only history never counts as a repeat
}

TEST(retry_naive_always_replay) {
  NaiveRetry nr;
  CHECK(std::string(nr.name()) == "naive_retry");
  for (int k = 1; k <= 3; ++k) {
    Choice c = nr.choose("push_A", {}, k);
    CHECK(c.strategy != nullptr);
    CHECK(c.strategy->name() == "replay");
  }
}

TEST(retry_corr_aware_first_choice_is_prior) {
  CorrelationAwareRetry rp;
  std::vector<Attempt> h = {fail("push_A", FailureMode::StoppedShort, "replay", {0.10, 0.10})};
  Choice c = rp.choose("push_A", h, 1);
  CHECK(c.strategy != nullptr);
  CHECK(c.strategy->name() == "obs_lead");   // empty library -> engineer's prior table
}

TEST(retry_corr_aware_never_repeats_failed_strategy) {
  CorrelationAwareRetry rp;
  std::vector<Attempt> h = {
      fail("push_A", FailureMode::StoppedShort, "replay", {0.10, 0.10}),
      fail("push_A", FailureMode::StoppedShort, "relocalize", {0.10, 0.10})};
  Choice c = rp.choose("push_A", h, 1);
  CHECK(c.strategy != nullptr);
  CHECK(std::string(c.strategy->name()) != "replay");
  CHECK(std::string(c.strategy->name()) != "relocalize");
}

TEST(retry_corr_aware_ladder_exhausted) {
  CorrelationAwareRetry rp;
  std::vector<Attempt> h;
  for (const char* s : kStrategyOrder) h.push_back(fail("push_A", FailureMode::StoppedShort, s, {0.10, 0.10}));
  Choice c = rp.choose("push_A", h, 1);
  CHECK(c.strategy == nullptr);      // all 7 strategies failed -> escalate to human
}

TEST(retry_record_learn_flag) {
  const std::string key = "push_A|off_axis_drift|relocalize";
  CorrelationAwareRetry learn;
  learn.record("push_A", FailureMode::OffAxisDrift, "relocalize", true);
  CHECK(learn.lib.count(key) == 1);
  CHECK(learn.lib[key].first == 1 && learn.lib[key].second == 1);

  CorrelationAwareRetry nolearn;
  nolearn.learn = false;
  nolearn.record("push_A", FailureMode::OffAxisDrift, "relocalize", true);
  CHECK(nolearn.lib.count(key) == 0);
}

TEST(retry_proactive_persistent_fix_and_new_episode) {
  CorrelationAwareRetry rp;
  CHECK(!rp.proactive("push_B"));    // nothing learned yet
  rp.record("push_A", FailureMode::StoppedShort, "relocalize", true);
  auto s = rp.proactive("push_B");   // shared-cause fix applied pro-actively
  CHECK(s != nullptr);
  CHECK(s->name() == "relocalize");
  rp.new_episode();
  CHECK(!rp.proactive("push_B"));    // cleared for the new episode
}

TEST(retry_save_load_roundtrip_and_missing_file) {
  CorrelationAwareRetry rp;
  rp.record("push_A", FailureMode::StoppedShort, "obs_lead", true);
  rp.record("push_A", FailureMode::StoppedShort, "replay", false);
  rp.record("push_B", FailureMode::OffAxisDrift, "relocalize", true);
  std::string path = "/tmp/relmw_retry_test_" + std::to_string(getpid()) + ".txt";
  CHECK(rp.save(path));

  CorrelationAwareRetry rp2;
  CHECK(rp2.load(path));
  CHECK(rp2.lib == rp.lib);          // identical map after round-trip

  CorrelationAwareRetry rp3;
  CHECK(!rp3.load("/tmp/relmw_retry_missing_" + std::to_string(getpid()) + "_nope.txt"));
  CHECK(rp3.lib.empty());
  std::remove(path.c_str());
}

TEST(retry_score_increases_with_successes) {
  CorrelationAwareRetry rp;
  const std::string sub = "push_A";
  FailureMode m = FailureMode::StoppedShort;
  double s0 = rp.score(sub, m, "obs_lead");
  rp.record(sub, m, "obs_lead", true);
  double s1 = rp.score(sub, m, "obs_lead");
  CHECK(s1 > s0);                    // more successes -> strictly higher posterior mean
  rp.record(sub, m, "obs_lead", true);
  double s2 = rp.score(sub, m, "obs_lead");
  CHECK(s2 > s1);
}

TEST_MAIN()
