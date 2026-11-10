# T3: POSIX (Linux host) platform layer + tests

Working directory: repository root. C++17, POSIX + pthreads only, no third-party libraries, no network installs.

## Goal
Implement the Linux/POSIX side of `include/relmw/platform.hpp` (DO NOT modify the header) in three files:
- `src/platform/posix/posix_timer.cpp`: `now_ns()` (CLOCK_MONOTONIC), `PeriodicTimer` (absolute-deadline
  `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`; first release = construction time + period; `wait_tick()` sleeps
  until next release, returns lateness = now - release (>=0), then advances release by one period; if more than one period
  late, skip ahead to the next future-or-current period boundary (no burst catch-up). `interrupt()` sets an atomic flag so the
  next or current wait returns -1 (a wait may finish its current sleep first; maximum extra delay = one period).
  `set_thread_sched()`: "fifo"/"rr" -> pthread_setschedparam with that priority; "other" -> SCHED_OTHER prio 0;
  "sporadic" -> not available on Linux: return false with *err = "SCHED_SPORADIC not supported on this OS".
  On EPERM return false with *err set (do not crash). `watchdog_attach()` -> return true (no-op, set nothing);
  `watchdog_heartbeat()` -> no-op.
- `src/platform/posix/posix_log.cpp`: `log_init(name)` stores the name; `log(level, fmt, ...)` writes ONE line to stderr:
  `[relmw <name>] <seconds since first log_init with 6 decimals> <LEVEL> <message>` where LEVEL is DEBUG/INFO/WARN/ERROR.
  Must be thread-safe (one mutex). Before log_init the name is "relmw".
- `src/platform/posix/posix_ctl.cpp`: `ControlSurface` emulating the QNX resource manager with files in directory
  `mount_` (created with mkdir -p semantics if missing): `start()` creates `<dir>/ctl` as a named FIFO (mkfifo, mode 0666;
  if it already exists as a FIFO reuse it), spawns one service thread that opens the FIFO (O_RDWR so it never sees EOF),
  reads lines, and for each non-empty line (strip trailing \r\n and spaces) calls `hooks_command(line)` and writes the
  reply plus "\n" atomically to `<dir>/reply` (write temp file + rename). Use poll() with 100 ms timeout so `stop()` can
  end the thread promptly (stop sets an atomic flag and joins; must be idempotent; destructor calls stop()).
  `publish()` writes `hooks_status()` to `<dir>/status` and `hooks_events()` to `<dir>/events`, each atomically (temp + rename).
  `start()` also calls publish() once. Use a pimpl struct `ControlSurface::Impl` (declared in the header) for state.

## Tests: `tests/test_posix_platform.cpp` (harness tests/minitest.hpp; do not modify it)
1. now_ns() is monotonic over 1000 calls and > 0.
2. PeriodicTimer(2 ms): 50 ticks take between 95 ms and 200 ms total; every lateness >= 0; median lateness < 2 ms.
3. interrupt(): a thread calls interrupt() after 20 ms while main thread loops on wait_tick() of a 5 ms timer; the loop ends
   (wait_tick returns -1) within 200 ms.
4. set_thread_sched("sporadic", ...) returns false and err mentions SPORADIC; set_thread_sched("other",0) returns true.
5. ControlSurface in a fresh temp dir (mkdtemp under /tmp): hooks: status returns "S1", events returns "E1", command echoes
   "OK " + line. After start(): files status == "S1", events == "E1". Write "fault bump\n" into <dir>/ctl (open O_WRONLY|O_NONBLOCK,
   write, close); within 1 s <dir>/reply contains "OK fault bump". Then change hooks' returned strings (use captured
   std::string variables) and call publish(): files updated. stop() returns within 500 ms; second stop() is harmless.
   Clean up the temp dir.
6. log(): calling log_init("t") then log(Level::Info, "x=%d", 5) does not crash (stderr content not checked).

## Acceptance command (must exit 0, no warnings)
```
zig-c++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itests src/platform/posix/posix_timer.cpp src/platform/posix/posix_log.cpp src/platform/posix/posix_ctl.cpp tests/test_posix_platform.cpp -o /tmp/relmw_test_posix -pthread && /tmp/relmw_test_posix
```
Run it 3 times; it must pass every time (no flaky timing).

## Constraints
Only create/modify the three files under `src/platform/posix/` and `tests/test_posix_platform.cpp`.
Wrap nothing in #ifdef __QNX__ (these files are only compiled on non-QNX). No busy-waiting.

## Final reply
Files changed + last 5 lines of the test output from the 3 runs.
