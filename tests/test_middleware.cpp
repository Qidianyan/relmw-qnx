// State-machine tests for ReliabilityMiddleware using a fake robot / policy / verifier (no simulator).
#include "minitest.hpp"
#include "relmw/middleware.hpp"
#include <map>

using namespace relmw;

namespace {
struct FakeRobot : RobotAdapter {
  Vec2 pose{0, 0};
  int steps = 0, rollbacks = 0;
  Obs get_obs() override { Obs o; o.pusher = pose; return o; }
  Obs step(Vec2 t) override { ++steps; pose = t; return get_obs(); }
  Vec2 robot_pose() override { return pose; }
  void move_robot_to(Vec2 p, int) override { ++rollbacks; pose = p; }
};
struct FakePolicy : Policy {
  Vec2 act(const Obs& o, const std::string&, const SubtaskMeta&, const Perturb&) override { return o.pusher + Vec2{0.001, 0}; }
};
// Verifier scripted per attempt: ok on attempt index >= ok_from (attempt counted by "start" checks).
struct Script {
  int attempt = -1;              // incremented by the middleware's start-of-attempt check
  int ok_from = 0;
  FailureMode mode = FailureMode::OffAxisDrift;
};
Subtask make(const std::string& name, Script* s, int max_steps = 20) {
  Subtask st;
  st.name = name; st.max_steps = max_steps; st.check_every = 5; st.hold = 1;
  st.verifier = [s](const Obs&) {
    Verdict v; v.has_obj = true; v.obj_xy = Vec2{0.1 * s->attempt, 0};
    v.ok = s->attempt >= s->ok_from;
    if (!v.ok) v.mode = s->mode;
    return v;
  };
  return st;
}
// Counts attempts through the RETRY / SUBTASK_START events.
struct Rec {
  std::map<EventType, int> n;
  std::vector<Script*> scripts;
  EventSink sink() {
    return [this](const Event& e) {
      n[e.type]++;
      if (e.type == EventType::SubtaskStart || e.type == EventType::Retry)
        for (auto* s : scripts) s->attempt++;
    };
  }
};
}  // namespace

TEST(always_ok_single_pass) {
  FakeRobot r; FakePolicy p; Script s; s.attempt = -1; s.ok_from = 0; Rec rec; rec.scripts = {&s};
  MiddlewareConfig c; c.max_retries = 3;
  ReliabilityMiddleware mw(p, r, nullptr, c, rec.sink());
  std::vector<Subtask> t{make("s1", &s)};
  auto rep = mw.run(t);
  CHECK(rep.success);
  CHECK(rep.attempts.size() == 1);
  CHECK(rep.n_failures() == 0);
  CHECK(r.rollbacks == 0);
  CHECK(rec.n[EventType::EpisodeDone] == 1);
}

TEST(recovers_after_two_failures) {
  FakeRobot r; FakePolicy p; Script s; s.ok_from = 2; Rec rec; rec.scripts = {&s};
  MiddlewareConfig c; c.max_retries = 3; c.final_check = false;
  ReliabilityMiddleware mw(p, r, nullptr, c, rec.sink());
  std::vector<Subtask> t{make("s1", &s)};
  auto rep = mw.run(t);
  CHECK(rep.success);
  CHECK(rep.attempts.size() == 3);
  CHECK(!rep.attempts[0].ok && !rep.attempts[1].ok && rep.attempts[2].ok);
  CHECK(rep.recovered());
  CHECK(r.rollbacks == 2);
  CHECK(rec.n[EventType::VerifyFail] == 2);
  CHECK(rec.n[EventType::Rollback] == 2);
  CHECK(rec.n[EventType::Retry] == 2);
  CHECK(rec.n[EventType::SubtaskOk] == 1);
  CHECK(rep.attempts[1].strategy == "replay");     // default retry policy = naive
}

TEST(retry_budget_exhausted) {
  FakeRobot r; FakePolicy p; Script s; s.ok_from = 99; Rec rec; rec.scripts = {&s};
  MiddlewareConfig c; c.max_retries = 3;
  ReliabilityMiddleware mw(p, r, nullptr, c, rec.sink());
  std::vector<Subtask> t{make("s1", &s)};
  auto rep = mw.run(t);
  CHECK(!rep.success);
  CHECK(rep.abort_reason == "retry_budget");
  CHECK(rep.failed_at == "s1");
  CHECK(rep.attempts.size() == 4);
  CHECK(r.rollbacks == 3);                          // no rollback after the last attempt
}

TEST(disabled_middleware_single_attempt) {
  FakeRobot r; FakePolicy p; Script s; s.ok_from = 99; Rec rec; rec.scripts = {&s};
  MiddlewareConfig c; c.max_retries = 0;
  ReliabilityMiddleware mw(p, r, nullptr, c, rec.sink());
  std::vector<Subtask> t{make("s1", &s)};
  auto rep = mw.run(t);
  CHECK(rep.attempts.size() == 1);
  CHECK(r.rollbacks == 0);
  CHECK(rep.abort_reason == "retry_budget");
}

TEST(unrecoverable_aborts_immediately) {
  FakeRobot r; FakePolicy p; Script s; s.ok_from = 99; s.mode = FailureMode::Unrecoverable; Rec rec; rec.scripts = {&s};
  MiddlewareConfig c; c.max_retries = 3;
  ReliabilityMiddleware mw(p, r, nullptr, c, rec.sink());
  std::vector<Subtask> t{make("s1", &s)};
  auto rep = mw.run(t);
  CHECK(rep.abort_reason == "unrecoverable");
  CHECK(rep.attempts.size() == 1);
}

TEST(time_budget) {
  FakeRobot r; FakePolicy p; Script s; s.ok_from = 99; Rec rec; rec.scripts = {&s};
  MiddlewareConfig c; c.max_retries = 3; c.episode_step_budget = 10;
  ReliabilityMiddleware mw(p, r, nullptr, c, rec.sink());
  std::vector<Subtask> t{make("s1", &s, 200)};
  auto rep = mw.run(t);
  CHECK(rep.abort_reason == "time_budget");
  CHECK(rep.attempts.size() == 1);
}

TEST(corr_aware_escalates_when_ladder_exhausted) {
  FakeRobot r; FakePolicy p; Script s; s.ok_from = 99; Rec rec; rec.scripts = {&s};
  CorrelationAwareRetry lib;
  MiddlewareConfig c; c.max_retries = 10;
  ReliabilityMiddleware mw(p, r, &lib, c, rec.sink());
  std::vector<Subtask> t{make("s1", &s)};
  auto rep = mw.run(t);
  CHECK(rep.abort_reason == "escalate_to_operator");
  CHECK(rec.n[EventType::Escalate] == 1);
  CHECK(rep.attempts.size() == 8);                  // first try + 7 distinct strategies
  std::vector<std::string> seen;
  for (size_t i = 1; i < rep.attempts.size(); ++i) {
    for (auto& x : seen) CHECK(x != rep.attempts[i].strategy);   // never repeats a failed strategy
    seen.push_back(rep.attempts[i].strategy);
  }
}

TEST(corr_aware_with_three_retries_is_retry_budget) {
  FakeRobot r; FakePolicy p; Script s; s.ok_from = 99; Rec rec; rec.scripts = {&s};
  CorrelationAwareRetry lib;
  MiddlewareConfig c; c.max_retries = 3;
  ReliabilityMiddleware mw(p, r, &lib, c, rec.sink());
  std::vector<Subtask> t{make("s1", &s)};
  auto rep = mw.run(t);
  CHECK(rep.abort_reason == "retry_budget");
  CHECK(!lib.lib.empty());                          // failures were recorded in the library
}

TEST(final_check_redoes_regressed_subtask) {
  FakeRobot r; FakePolicy p; Script a; a.ok_from = 0; Script b; b.ok_from = 0; Rec rec; rec.scripts = {&a, &b};
  MiddlewareConfig c; c.max_retries = 3;
  ReliabilityMiddleware mw(p, r, nullptr, c, rec.sink());
  std::vector<Subtask> t{make("s1", &a), make("s2", &b)};
  bool broken = true;
  // s1's postcondition is "regressed" exactly once after s2 runs (final check sees it), then fixed by the redo.
  auto base = t[0].verifier;
  int calls_after_s2 = 0;
  t[0].verifier = [&](const Obs& o) {
    Verdict v = base(o);
    if (rec.n[EventType::SubtaskOk] >= 2 && broken && calls_after_s2++ == 0) { v.ok = false; v.mode = FailureMode::OffAxisDrift; broken = false; }
    return v;
  };
  auto rep = mw.run(t);
  CHECK(rep.success);
  bool saw_regressed = false, saw_redo = false;
  for (auto& at : rep.attempts) {
    if (at.mode == FailureMode::Regressed) saw_regressed = true;
    if (at.strategy.find("@redo") != std::string::npos) saw_redo = true;
  }
  CHECK(saw_regressed);
  CHECK(saw_redo);
}

TEST_MAIN()
