#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
# test_supervisor_e2e.sh - end-to-end test of the relmw supervisor control surface.
#
#   sh tests/test_supervisor_e2e.sh <relmw_supervisor-binary> <relmwctl-script>
#
# Starts the supervisor on a private mount, drives it through relmwctl (faults, quit), checks the
# failure library is persisted after every mw episode and survives a restart.
# Never uses pkill/killall: the supervisor is stopped only by its own PID.

set -u

SUP=${1:-}
CTL=${2:-}

D=$(mktemp -d) || { echo "test_supervisor_e2e: FAIL: mktemp failed" >&2; exit 1; }
LOG=$D/sup.log
PID=

cleanup() {
  if [ -n "$PID" ]; then kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; fi
  rm -rf "$D"
}
trap 'exit 1' INT TERM
trap cleanup EXIT

fail() {
  echo "test_supervisor_e2e: FAIL: $*" >&2
  echo "--- last 20 lines of supervisor log ($LOG) ---" >&2
  tail -n 20 "$LOG" >&2 2>/dev/null
  exit 1
}

# Wait up to 10 s for $1 to exit; returns 0 and reaps it, or 124 on timeout.
wait_exit() {
  i=0
  while [ $i -lt 100 ]; do
    kill -0 "$1" 2>/dev/null || break
    st=$(ps -o stat= -p "$1" 2>/dev/null)
    case $st in Z*|'') break;; esac
    sleep 0.1
    i=$((i + 1))
  done
  [ $i -lt 100 ] || return 124
  wait "$1" 2>/dev/null
}

start_supervisor() {
  "$SUP" --episodes 0 --period-us 300 --sched other --mount "$D/mnt" --library "$D/lib.txt" --cpu 0 >>"$LOG" 2>&1 &
  PID=$!
}

[ -x "$SUP" ] || fail "supervisor binary not found or not executable: $SUP"
[ -x "$CTL" ] || fail "relmwctl not found or not executable: $CTL"

# 1) boot on a private mount; the control surface must come up
start_supervisor
i=0
while [ ! -f "$D/mnt/status" ] && [ $i -lt 100 ]; do
  kill -0 "$PID" 2>/dev/null || fail "supervisor exited before publishing status"
  sleep 0.1
  i=$((i + 1))
done
[ -f "$D/mnt/status" ] || fail "status node did not appear within 10 s"

# 1b) --cpu 0 pins the CONTROL thread (the control-surface service thread was created earlier and keeps the default
# affinity); the status line must report it.
i=0
while [ $i -lt 50 ]; do
  grep -q '^control_affinity=_cpu-0 ' "$D/mnt/status" 2>/dev/null && break
  kill -0 "$PID" 2>/dev/null || fail "supervisor exited before publishing control_affinity"
  sleep 0.1
  i=$((i + 1))
done
grep -q '^control_affinity=_cpu-0 cpus=0 mask=0x1$' "$D/mnt/status" || fail "status has no 'control_affinity=_cpu-0 cpus=0 mask=0x1' line (affinity must be the CONTROL thread's)"

# 2) valid and invalid fault commands through relmwctl
"$CTL" -m "$D/mnt" fault percep_bias >/dev/null 2>&1
[ $? -eq 0 ] || fail "relmwctl fault percep_bias did not exit 0"
"$CTL" -m "$D/mnt" fault bogus >/dev/null 2>&1
[ $? -eq 1 ] || fail "relmwctl fault bogus did not exit 1"

# 3) episodes must run and the mw arm must persist its failure library after every episode
eps=0
i=0
while [ $i -lt 300 ]; do
  eps=$(grep -oE 'episodes=[0-9]+' "$D/mnt/status" 2>/dev/null | head -n 1 | cut -d= -f2)
  case ${eps:-0} in ''|*[!0-9]*) eps=0;; esac
  [ "$eps" -ge 3 ] && break
  kill -0 "$PID" 2>/dev/null || fail "supervisor died before 3 episodes"
  sleep 0.2
  i=$((i + 1))
done
[ "$eps" -ge 3 ] || fail "status never showed episodes>=3 (last: episodes=$eps)"
[ -s "$D/lib.txt" ] || fail "failure library $D/lib.txt missing or empty after $eps episodes"

# 4) quit must stop the supervisor promptly
"$CTL" -m "$D/mnt" quit >/dev/null 2>&1
[ $? -eq 0 ] || fail "relmwctl quit did not exit 0"
wait_exit "$PID"
rc=$?
[ $rc -eq 124 ] && fail "supervisor did not exit within 10 s of quit"
[ $rc -eq 0 ] || fail "supervisor exited with status $rc after quit"
PID=

# 5) restart with the same --library: entries must have survived the restart
start_supervisor
ents=0
i=0
while [ $i -lt 100 ]; do
  ents=$(grep -oE 'failure_library_entries=[0-9]+' "$D/mnt/status" 2>/dev/null | head -n 1 | cut -d= -f2)
  case ${ents:-0} in ''|*[!0-9]*) ents=0;; esac
  [ "$ents" -gt 0 ] && break
  kill -0 "$PID" 2>/dev/null || fail "supervisor died after restart"
  sleep 0.1
  i=$((i + 1))
done
[ "${ents:-0}" -gt 0 ] || fail "failure_library_entries not > 0 within 10 s after restart (entries=$ents)"

# 5b) plan command: reorder takes effect from the NEXT episode; invalid plans are rejected
rep=$("$CTL" -m "$D/mnt" plan push_B,push_A)
[ $? -eq 0 ] || fail "relmwctl plan push_B,push_A did not exit 0"
case "$rep" in
  "OK plan push_B,push_A") ;;
  *) fail "unexpected plan reply: $rep" ;;
esac
"$CTL" -m "$D/mnt" plan push_C >/dev/null 2>&1
[ $? -eq 1 ] || fail "relmwctl plan push_C did not exit 1"
i=0
plan_seen=0
while [ $i -lt 80 ]; do
  ev=$("$CTL" -m "$D/mnt" events 50 2>/dev/null)
  if printf '%s\n' "$ev" | grep -q 'SUBTASK_START push_B' && \
     printf '%s\n' "$ev" | awk '
        /SUBTASK_START push_B/ { b = $1 }
        /SUBTASK_START push_A/ { if (b != "" && b == $1) { print "ok"; exit 0 } }
      ' | grep -q ok; then
    plan_seen=1
    break
  fi
  kill -0 "$PID" 2>/dev/null || fail "supervisor died while waiting for the planned episode"
  sleep 0.2
  i=$((i + 1))
done
[ $plan_seen -eq 1 ] || fail "no episode ran SUBTASK_START push_B before SUBTASK_START push_A under plan push_B,push_A"
grep -q '^plan=push_B,push_A$' "$D/mnt/status" || fail "status has no 'plan=push_B,push_A' line"

"$CTL" -m "$D/mnt" quit >/dev/null 2>&1
[ $? -eq 0 ] || fail "relmwctl quit (after restart) did not exit 0"
wait_exit "$PID"
rc=$?
[ $rc -eq 124 ] && fail "supervisor did not exit within 10 s of quit (restart)"
[ $rc -eq 0 ] || fail "supervisor exited with status $rc after quit (restart)"
PID=

echo "test_supervisor_e2e: OK"
exit 0
