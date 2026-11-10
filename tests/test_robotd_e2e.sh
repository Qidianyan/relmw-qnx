#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
# test_robotd_e2e.sh - end-to-end test of the robot driver process (checklist C6).
#
#   sh tests/test_robotd_e2e.sh <relmw_robotd-binary> <relmw_supervisor-binary>
#
#  1. relmw_robotd on a private socket; `relmw_supervisor --robot` must agree with an in-process run
#     (same seeds, same arm) on the episodes=/success=/... line and on failure_library_entries.
#  2. Crash test: robotd started with --quit-after-requests dies mid-run; the supervisor reconnects
#     (retry every 200 ms), re-runs the SAME episode seed, and finishes with episodes=8,
#     robot_link_errors>=1, exit 0, and the SAME counters as the in-process run.
# Never uses pkill/killall: only PIDs recorded by this script are killed.
set -u

ROBOTD=${1:-}
SUP=${2:-}

D=$(mktemp -d) || { echo "test_robotd_e2e: FAIL: mktemp failed" >&2; exit 1; }
PIDS=

cleanup() {
  for p in $PIDS; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
  rm -rf "$D"
}
trap 'exit 1' INT TERM
trap cleanup EXIT

fail() {
  echo "test_robotd_e2e: FAIL: $*" >&2
  echo "--- last 20 lines of robotd log ($D/robotd.log) ---" >&2
  tail -n 20 "$D/robotd.log" >&2 2>/dev/null
  exit 1
}

start_robotd() {   # $1 = socket path, $2 = extra args (may be empty)
  "$ROBOTD" --path "$1" ${2:-} >>"$D/robotd.log" 2>&1 &
  PIDS="$PIDS $!"
}

wait_sock() {   # up to 10 s for the SEQPACKET socket file
  i=0
  while [ ! -S "$1" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
  [ -S "$1" ] || fail "robotd socket $1 did not appear within 10 s"
}

# Print the first line of $1 matching $2 (fail if absent) into stdout.
grep_line() {
  grep -m1 -E "$2" "$1" 2>/dev/null || fail "no line matching '$2' in $1"
}

[ -x "$ROBOTD" ] || fail "relmw_robotd not found or not executable: $ROBOTD"
[ -x "$SUP" ] || fail "relmw_supervisor not found or not executable: $SUP"

# ---- 1) remote driver vs in-process: same seeds + arm => same counters ----
SOCK=$D/r.sock
start_robotd "$SOCK"
wait_sock "$SOCK"
"$SUP" --episodes 6 --arm mw --sched other --period-us 300 --mount "$D/m1" --library "$D/lib_remote.txt" --robot "$SOCK" >"$D/remote.out" 2>"$D/remote.err"
rc=$?
[ $rc -eq 0 ] || fail "supervisor (remote) exited $rc"
"$SUP" --episodes 6 --arm mw --sched other --period-us 300 --mount "$D/m2" --library "$D/lib_local.txt" >"$D/local.out" 2>"$D/local.err"
rc=$?
[ $rc -eq 0 ] || fail "supervisor (in-process) exited $rc"
grep_line "$D/remote.out" '^episodes=[0-9]+ ' >"$D/a"
grep_line "$D/local.out"  '^episodes=[0-9]+ ' >"$D/b"
cmp "$D/a" "$D/b" || fail "counter lines differ between --robot and in-process runs"
grep_line "$D/remote.out" '^failure_library_entries=[0-9]+$' >"$D/c"
grep_line "$D/local.out"  '^failure_library_entries=[0-9]+$' >"$D/d"
cmp "$D/c" "$D/d" || fail "failure_library_entries differ between --robot and in-process runs"
# reviewer addition: the learned library itself must be byte-identical, not just its size
cmp "$D/lib_remote.txt" "$D/lib_local.txt" || fail "failure library contents differ between --robot and in-process runs"
echo "test_robotd_e2e: part 1 OK (remote driver == in-process)"

# ---- 2) crash + process-level recovery over 8 episodes ----
SOCK2=$D/r2.sock
RPID=
restarts=0
start_crashy() {
  # Calibrated: an 8-episode mw run issues ~2700 robot-link requests, so 1200 per instance guarantees
  # a mid-episode crash (>= 1 link error) while 4 instances (3 restarts) still cover the whole run.
  "$ROBOTD" --path "$SOCK2" --quit-after-requests 1200 >>"$D/robotd_crash.log" 2>&1 &
  RPID=$!
  PIDS="$PIDS $RPID"
}
start_crashy
wait_sock "$SOCK2"
"$SUP" --episodes 8 --arm mw --sched other --period-us 300 --mount "$D/m3" --library "$D/lib_crash.txt" --robot "$SOCK2" >"$D/crash.out" 2>"$D/crash.err" &
SPID=$!
PIDS="$PIDS $SPID"
# restart robotd whenever it exits (max 3 restarts) while the supervisor runs
while kill -0 "$SPID" 2>/dev/null; do
  if ! kill -0 "$RPID" 2>/dev/null; then
    wait "$RPID" 2>/dev/null
    [ $restarts -lt 3 ] || fail "robotd needed more than 3 restarts"
    restarts=$((restarts + 1))
    echo "test_robotd_e2e: robotd exited (simulated crash); restart #$restarts"
    start_crashy
  fi
  sleep 0.1
done
wait "$SPID"
rc=$?
[ $rc -eq 0 ] || fail "supervisor (crash test) exited $rc"
grep_line "$D/crash.out" '^episodes=8 ' >/dev/null
errs=$(grep_line "$D/crash.out" '^robot_link_errors=[0-9]+$' | cut -d= -f2)
case ${errs:-} in ''|*[!0-9]*) errs=0;; esac
[ "$errs" -ge 1 ] || fail "robot_link_errors=$errs, expected >= 1 after driver crashes"
# same counters as an in-process run of the same 8 episodes
"$SUP" --episodes 8 --arm mw --sched other --period-us 300 --mount "$D/m4" --library "$D/lib_base.txt" >"$D/base.out" 2>/dev/null
rc=$?
[ $rc -eq 0 ] || fail "supervisor (baseline) exited $rc"
grep_line "$D/crash.out" '^episodes=[0-9]+ ' >"$D/e"
grep_line "$D/base.out"  '^episodes=[0-9]+ ' >"$D/f"
cmp "$D/e" "$D/f" || fail "crash-test counters differ from the in-process baseline"
cmp "$D/lib_crash.txt" "$D/lib_base.txt" || fail "failure library after crash recovery differs from the in-process baseline"
echo "test_robotd_e2e: part 2 OK (recovered from robotd crash x$restarts; robot_link_errors=$errs)"

echo "test_robotd_e2e: OK"
exit 0
