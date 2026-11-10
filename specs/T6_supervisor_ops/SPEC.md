# T6: supervisor operability — persistent failure library, time-based heartbeat, `relmwctl`, end-to-end test

Working directory: repository root. C++17. Read first: apps/relmw_supervisor.cpp, include/relmw/{platform,retry_policy}.hpp,
src/platform/posix/posix_ctl.cpp (POSIX control surface: `<dir>/status`, `<dir>/events` files, `<dir>/ctl` FIFO, replies in
`<dir>/reply`), src/core/retry_policy.cpp (save/load).
On QNX the same surface is a resource manager: `/dev/relmw/{status,events,ctl}`, the reply to the last command is read back
with `cat /dev/relmw/ctl`. Do NOT touch src/platform/qnx/ or include/relmw/platform.hpp.

## Changes in `apps/relmw_supervisor.cpp`
1. Failure library persistence (checklist D5): when `--library FILE` is given and the arm of the finished episode is `mw`,
   save the library after every episode ATOMICALLY: `lib.save(FILE + ".tmp")` then `std::rename()` over FILE. Log on failure.
   On start the existing load stays. Add the line `failure_library_file=<FILE or ->` to status_text().
2. Heartbeat by time, not by tick count: keep `last_hb_ns`; call `pf::watchdog_heartbeat()` whenever
   `now - last_hb_ns >= heartbeat_period/2` (heartbeat_period = 100 ms, a named constant). Call the helper both from
   RealtimeRobot::step() and from the paused-wait loop in main(), so a paused supervisor still heartbeats.
3. New flag `--max-episodes-per-run`? NO. Keep flags unchanged except: add `--heartbeat-ms N` (default 100) used by (2) and
   by the watchdog_attach() call.
4. Keep the file header usage comment in sync.

## New file `scripts/relmwctl` (POSIX sh, must run under QNX ksh and Linux dash/bash; chmod +x)
```
relmwctl [-m DIR] status | events [N] | fault <none|bump|percep_bias|slippery|random|nominal> | arm <mw|naive|none>
         | knock | pause | resume | reset-stats | quit | watch [SECONDS]
```
- DIR default: `$RELMW_MOUNT`, else `/dev/relmw` if it exists, else `/tmp/relmw`.
- status/events: cat the node (events N: last N lines, default 20). watch: print status every SECONDS (default 1) until Ctrl-C.
- commands: `echo "<cmd> <arg>" > DIR/ctl`; then print the reply: if `DIR/ctl` is a FIFO (`[ -p ]`, POSIX host) wait up to
  2 s for `DIR/reply` to change (compare content/mtime before/after) and print it; otherwise (QNX resmgr) `cat DIR/ctl`.
  Exit 0 if the reply starts with `OK`, 1 otherwise; exit 2 on usage errors. No bashisms (check with `sh -n` and, if present, `checkbashisms`/`shellcheck -s sh`).

## New test `tests/test_supervisor_e2e.sh` registered in CMakeLists.txt
Register with: `add_test(NAME test_supervisor_e2e COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_supervisor_e2e.sh $<TARGET_FILE:relmw_supervisor> ${CMAKE_CURRENT_SOURCE_DIR}/scripts/relmwctl)`
inside `if(NOT RELMW_QNX)`. The script:
- makes a temp dir (mktemp -d), starts `relmw_supervisor --episodes 0 --period-us 300 --sched other --mount $D/mnt --library $D/lib.txt`
  in the background, records its PID with `$!`; a `trap` kills ONLY that PID (never pkill/killall) and removes the temp dir.
- waits (max 10 s) for `$D/mnt/status`; `relmwctl -m $D/mnt fault percep_bias` must exit 0; `relmwctl -m $D/mnt fault bogus` must exit 1.
- waits until status shows episodes>=3 (max 60 s), checks `$D/lib.txt` exists and is non-empty.
- `relmwctl -m $D/mnt quit`, waits for the PID to exit (max 10 s).
- restarts the supervisor with the same --library and checks that status shows `failure_library_entries=` with a value > 0
  within 10 s (library survived the restart), then quits it the same way.
- prints `test_supervisor_e2e: OK` and exits 0; any failure prints the reason and the last 20 lines of the supervisor log and exits 1.
- Total runtime must stay < 60 s on a loaded machine.

## Acceptance (exit 0, zero warnings)
```
rm -rf /tmp/relmw_t6 && CC=gcc CXX=g++ cmake -S . -B /tmp/relmw_t6 -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build /tmp/relmw_t6 2>&1 | grep -iE "warning|error" ; ctest --test-dir /tmp/relmw_t6 --output-on-failure | tail -4 && sh -n scripts/relmwctl
```
Run the e2e test 3 times in a row; it must pass every time. Budget: 20 minutes. Write files into the repo first, then iterate.
Never use pkill/killall; stop processes only by PID.

## Final reply
Diff summary, e2e output, timings of the 3 runs.
