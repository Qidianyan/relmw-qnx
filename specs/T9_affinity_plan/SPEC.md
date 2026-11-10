# T9: pin the control thread to a CPU cluster (checklist C8) + `plan` control command (checklist F2, supervisor side)

Working directory: repository root (C++17, CMake+Ninja). Read first: include/relmw/platform.hpp,
src/platform/posix/posix_timer.cpp (set_thread_sched), apps/relmw_supervisor.cpp, apps/relmw_robotd.cpp,
src/core/experiment.cpp (make_task: subtasks "push_A", "push_B"), include/relmw/experiment.hpp, tests/test_posix_platform.cpp,
tests/test_supervisor_e2e.sh, scripts/relmwctl (already has `plan`), docs/llm_assist.md, CMakeLists.txt.
Do NOT touch src/platform/qnx/, tools/qnx_stubs/, scripts/qnx_*.sh, results/. The reviewer writes the QNX implementation of
the new platform function (QNX 8.0: pthread_cluster_set(0, name) with the predefined clusters "_all" and "_cpu-N").

## Part A — CPU affinity (C8)
1. `include/relmw/platform.hpp`: add, next to set_thread_sched:
   ```
   // CPU affinity of the CALLING thread, by QNX cluster name: "_all" (every CPU), "_cpu-N" (CPU N, 0-based); on QNX also
   // custom clusters defined by the startup program (-c). POSIX host: "_all" and "_cpu-N" only (pthread_setaffinity_np).
   bool set_thread_cluster(const std::string& name, std::string* err = nullptr);
   // Human-readable affinity of the calling thread, e.g. "cpus=1 mask=0x2" (POSIX: from pthread_getaffinity_np).
   std::string thread_affinity_text();
   ```
2. POSIX implementation in src/platform/posix/posix_timer.cpp (needs `_GNU_SOURCE` for the affinity API; keep the file
   compiling warning-free). Parse strictly: "_all" or "_cpu-" followed by decimal digits only (no sign, no spaces, no
   trailing text); N must be < number of configured CPUs (sysconf(_SC_NPROCESSORS_CONF)) and allowed by the process mask
   (sched_getaffinity of pid 0 at first call — e.g. inside a container): otherwise false + err "EINVAL ...". "_all" = the
   process's initial allowed mask (captured once), not blindly every CPU. Unknown names -> false + err explaining that custom
   clusters exist only on QNX.
3. Supervisor: flags `--cpu N` (= `--cluster _cpu-N`) and `--cluster NAME`. Apply to the CONTROL thread only (main thread,
   right after set_thread_sched and before PeriodicTimer is created). The control-surface service thread is created earlier
   and must stay on the default affinity (verify that in the test, see below). Warn and continue on failure. Status gets a
   line `control_affinity=<cluster or -> <thread_affinity_text()>`; the --report JSON gets `"cluster":"<name or ->"`.
   Header usage comment updated.
4. relmw_robotd: same `--cpu N` / `--cluster NAME` for its serving thread (call before robot_serve).
5. Tests: extend tests/test_posix_platform.cpp (or a new tests/test_affinity.cpp registered like the others; it must NOT
   be skipped on QNX unless it uses POSIX-only calls — put POSIX-only checks under `#ifndef __QNX__`):
   - "_cpu-0" succeeds; afterwards sched_getcpu() == 0 over 1000 yields; thread_affinity_text() mentions cpu 0.
   - "_all" restores the initial mask (compare with CPU_EQUAL).
   - rejects: "", "_cpu-", "_cpu--1", "_cpu-1x", "_cpu- 1", "_cpu-99999", "big", "_cpu-<N_conf>"; err non-empty each time.
   - if the host has >= 2 allowed CPUs: "_cpu-1" works and a thread created AFTER the call in that thread inherits it
     (document: Linux threads inherit affinity; on QNX the inherit mask differs — reviewer handles QNX), while a thread
     created BEFORE keeps its old mask. Skip that case with a printed note when only one CPU is allowed.
   - supervisor: in tests/test_supervisor_e2e.sh start with `--cpu 0` and check the status line `control_affinity=_cpu-0`.

## Part B — `plan` control command (F2, supervisor side; the LLM tool already produces `plan push_B,push_A`)
1. Core (src/core/experiment.cpp + header): `bool apply_plan(std::vector<Subtask>& task, const std::vector<std::string>& plan,
   std::string* err)` — reorders/filters `task` to the plan order. Valid plan: non-empty, no duplicates, every name exists in
   `task`. On error: task unchanged, false. And `bool parse_plan(const std::string& csv, std::vector<std::string>& out,
   std::string* err)` — comma-separated, no empty items, no spaces, names `[A-Za-z0-9_]+`, max 8 items.
2. Supervisor: ctl command `plan <csv>` (validated with parse_plan + a dry apply_plan against make_task's names) -> stored
   in Shared, takes effect from the NEXT episode, reply `OK plan push_B,push_A`, event `CTL plan push_B,push_A (from next
   episode)`; `plan default` restores the built-in order; invalid -> `ERR plan: <reason>`. Also `--plan CSV` flag at start.
   Status line `plan=<csv or default>`. With the default plan nothing changes (the benchmark and the default supervisor
   output stay byte-identical; apply_plan is not called when the plan is default).
   Note for correctness: the middleware's FINAL CHECK re-verifies every subtask in the task vector, so a filtered plan
   ("push_A" only) also re-checks only push_A; the end-of-episode ground-truth score in the supervisor must then also only
   score the planned objects — implement that (score objects whose subtask is in the plan).
3. Tests: unit tests for parse_plan/apply_plan (new tests/test_plan.cpp registered in CMake: valid reorder, filter, dup,
   unknown, empty, spaces, too many, task unchanged on error); e2e: in tests/test_supervisor_e2e.sh send
   `relmwctl plan push_B,push_A` -> exit 0, `relmwctl plan push_C` -> exit 1, then wait until `relmwctl events 50` shows a
   `SUBTASK_START push_B` line that comes before the next `SUBTASK_START push_A` line of the same episode, and status shows
   `plan=push_B,push_A`. Keep e2e < 60 s.
4. docs/llm_assist.md: update the "supervisor side" paragraph to say the command is implemented, with the exact reply.

## Acceptance (exit 0, zero warnings with g++)
```
rm -rf /tmp/relmw_t9 && CC=gcc CXX=g++ cmake -S . -B /tmp/relmw_t9 -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build /tmp/relmw_t9 2>&1 | grep -iE "warning|error" ; ctest --test-dir /tmp/relmw_t9 --output-on-failure | tail -4
/tmp/relmw_t9/relmw_bench --n 100 --out /tmp/relmw_t9_bench && cmp /tmp/relmw_t9_bench/episodes.jsonl results/host_linux/episodes.jsonl && cmp /tmp/relmw_t9_bench/summary.json results/host_linux/summary.json && echo BITIDENTICAL
```
Run the supervisor e2e 3 times; must pass each time. SPDX MIT + "Copyright (c) 2026 elidot" headers on new files. No git
commits. Never pkill/killall. Budget: 25 minutes.

## Final reply
Files + one line each, test tail, BITIDENTICAL line, e2e timings, what the host CPU count was, anything not done.
