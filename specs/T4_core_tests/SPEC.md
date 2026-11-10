# T4: Unit tests for the portable core

Working directory: repository root. C++17. Use the harness `tests/minitest.hpp` (TEST / CHECK / CHECK_NEAR / TEST_MAIN()).
DO NOT modify anything under `include/` or `src/` — this task ONLY adds tests. If you believe you found a bug in src/,
do not fix it: describe it precisely in your final reply (file, line, failing input) and write the test so it documents
the current behaviour with a comment `// SUSPECTED BUG:`.

Read these first: include/relmw/{types,rng,robot,sim2d,policy,verifier,strategies,retry_policy,middleware,experiment}.hpp
and the matching src/core/*.cpp.

## Files to create
1. `tests/test_rng.cpp`
   - same seed -> identical first 1000 next_u64(); different seeds differ; uniform() in [0,1) over 100k draws with mean in
     [0.49,0.51]; normal(0,1) over 100k draws mean |m|<0.02 and variance in [0.97,1.03]; randint(3,7) only returns 3..6 and
     hits each value; choice_weighted with weights {0,1,0} always returns 1; hash_mix({1,2}) != hash_mix({2,1}) and is stable
     (call twice, equal). Also pin golden values: record `Rng(42).next_u64()` printed value as a constant in the test
     (compute once and hard-code it) so future changes to the RNG (which would break Linux/QNX result parity) are caught.
2. `tests/test_sim2d.cpp`
   - nominal sim (SimConfig{} default) has fault None for seeds 0..199; field sim (field_faults=true) over seeds 0..999 draws
     all four faults with frequencies within ±0.06 of the configured weights.
   - determinism: two PushSim2D(123, field) driven by the same 300 targets give identical true_state() each tick.
   - pushing: start at seed 7 nominal; drive the pusher with expert_action(true obs, obj 0, goal_of(0), 0.0) for 200 ticks;
     final distance of cube A to goal_of(0) < kGoalTol.
   - percep_bias: with force_fault=2 and field_faults=true, get_obs().obj[i] - true_state().obj[i] has norm in [0.04,0.07] and
     is the same vector for both objects; with no fault it is zero.
   - knock(): moves the object by exactly the delta; move_robot_to(p) ends within 0.01 m of p when the path is free
     (use a target far from both cubes, e.g. (0,-0.35)).
   - slippery (force_fault=3): friction() < 0.15 * 0.6 * 1.15 + 1e-12.
3. `tests/test_retry_policy.cpp`
   - is_repeat(): same subtask & mode & end_obj within 0.02 -> true; end_obj 0.05 apart -> false; different mode -> false;
     ok attempts in history are skipped.
   - NaiveRetry::choose always returns "replay".
   - CorrelationAwareRetry: with an empty library, after a StoppedShort failure the first choice is "obs_lead" (prior);
     never returns a strategy whose name is already in failed history strategies; when all 7 are used -> nullptr strategy.
   - record(): with learn=true adds counts under key "push_A|off_axis_drift|relocalize"; with learn=false does not.
   - a successful "relocalize" record makes proactive() return a strategy named "relocalize"; new_episode() clears it.
   - save()/load() round-trip to a temp file in /tmp (unique name) gives an identical map; load() of a missing file returns false.
   - score(): more successes for a (subtask,mode,strategy) strictly increases its score.
4. `tests/test_middleware.cpp`
   - Build a tiny fake RobotAdapter + fake Policy + verifier lambdas (no simulator) to test the state machine exactly:
     a) verifier always ok -> run() success, 1 attempt per subtask, zero failures.
     b) verifier fails the first 2 evaluations of subtask "s1" then ok (count calls; return ok on/after the 3rd check of a
        new attempt as you design it) with max_retries=3 -> success, attempts show failures then ok, robot.move_robot_to was
        called once per failed attempt (rollback), events emitted include VERIFY_FAIL, ROLLBACK, RETRY, SUBTASK_OK, EPISODE_DONE.
     c) verifier never ok with NaiveRetry and max_retries=3 -> success false, abort_reason "retry_budget", 4 attempts.
     d) max_retries=0 -> exactly 1 attempt, no rollback, abort "retry_budget".
     e) mode Unrecoverable -> abort "unrecoverable" after the first attempt.
     f) episode_step_budget small (e.g. 10 with max_steps 200) -> abort "time_budget" on the second attempt.
     g) CorrelationAwareRetry where all strategies fail -> after max_retries the reason is "retry_budget" (max_retries=3) and
        with max_retries=10 it is "escalate_to_operator".
5. `tests/test_experiment.cpp`
   - run_episode(1000, false, Arm::SinglePass, nullptr, cfg) succeeds (gt_success true).
   - determinism: run_benchmark with n=20, warmup=5 twice -> identical summary_json strings.
   - run_benchmark n=100 warmup=40 (defaults): parse table text; field corr_aware_retry success >= field naive_retry success
     >= field single_pass success; nominal single_pass success >= 95. summary_json contains "\"vs_single_pass\"".
     Whole test must take < 10 s.

## Acceptance command (exit 0, no warnings)
```
R=$(pwd); rm -rf /tmp/relmw_t4 && mkdir /tmp/relmw_t4 && for t in test_rng test_sim2d test_retry_policy test_middleware test_experiment; do zig-c++ -std=c++17 -O2 -Wall -Wextra -I$R/include -I$R/tests $R/src/core/*.cpp $R/tests/$t.cpp -o /tmp/relmw_t4/$t && /tmp/relmw_t4/$t | tail -1 || exit 1; done
```
(Run from the repository root; `zig-c++` is on PATH.)

## Final reply
Files created, the per-test summary lines, and any SUSPECTED BUG findings.
