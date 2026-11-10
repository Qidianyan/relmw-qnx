# T5: prove the real-time path does not allocate (checklist B9)

Working directory: repository root. C++17, harness `tests/minitest.hpp` (TEST / CHECK / TEST_MAIN()).
DO NOT modify anything under `include/`, `src/` or `apps/`. This task ONLY adds one test file (and one name in CMakeLists.txt).
If something on the real-time path DOES allocate, do not fix it: write the test so it measures and prints the count,
mark the assertion with `// FINDING:` and report it (file, function, count) in your final reply.

Read first: include/relmw/{latency,sim2d,platform}.hpp, src/platform/latency.cpp, src/core/sim2d.cpp, src/platform/posix/posix_timer.cpp.

## File to create: `tests/test_rt_noalloc.cpp`
- Replace global `operator new(size_t)`, `operator new[](size_t)`, `operator delete(void*)`, `operator delete[](void*)`,
  and the sized-delete variants in the test TU; count calls with a `static std::atomic<long>` that is only incremented
  while a thread_local flag `g_counting` is true. Provide RAII `struct CountAllocs { long n() const; }` that turns counting on
  for the current thread.
- Cases (at least 5):
  1. `LatencyStats::record()` x 100000 (values 0..10 ms incl. > deadline) -> 0 allocations. Also quantile()/max()/deadline_misses() -> 0.
  2. `PushSim2D::step(target)` x 2000 after 10 warm-up steps, nominal and field (force_fault 1,2,3) -> 0 allocations.
  3. `PushSim2D::get_obs()` / `true_state()` / `robot_pose()` x 2000 -> 0 allocations.
  4. `platform::PeriodicTimer(1 ms)::wait_tick()` x 100 after the constructor (POSIX impl) -> 0 allocations.
  5. `platform::now_ns()` x 10000 -> 0 allocations.
  6. Document (do not assert 0) the count for one full `ReliabilityMiddleware::run()` episode, printed with printf, to show
     allocations happen only at subtask/attempt boundaries (event strings), not per tick: run one nominal episode and print
     total allocations and allocations per sim tick (expect << 1).
- Append `test_rt_noalloc` to the `foreach(t ...)` list in CMakeLists.txt (no other CMake edits).

## Acceptance (exit 0, zero warnings)
```
rm -rf /tmp/relmw_t5 && CC=gcc CXX=g++ cmake -S . -B /tmp/relmw_t5 -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build /tmp/relmw_t5 2>&1 | grep -iE "warning|error" ; ctest --test-dir /tmp/relmw_t5 --output-on-failure | tail -3
```
All tests must pass (the existing 8 plus the new one). Budget: 15 minutes. Write the file into the repo first, then iterate.

## Final reply
Per-case results, the printed per-episode allocation numbers, and any FINDING.
