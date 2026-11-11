#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
# test_sim_e2e.sh BUILD_DIR SRC_DIR - end to end over the QNX API simulation on Linux (not QNX).
# The QNX sources (src/platform/qnx: pulse timer, /dev/relmw resource manager, /dev/robot0 driver link, slog2) run
# against compat/qnx_sim. Proves the QNX-layer LOGIC only; no timing, scheduling or security claim about QNX.
set -u
B=${1:?build dir}
S=${2:?source dir}
D=$(mktemp -d) || exit 1
export QNXSIM_ROOT=$D/root
export RELMW_QNX_SIM=1 QNXSIM_IO=$B/qnxsim_io
CTL="sh $S/scripts/relmwctl -m /dev/relmw_e2e"
PID=
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$D"; }
trap cleanup EXIT
trap 'exit 1' INT TERM
fail() { echo "test_sim_e2e: FAIL: $*" >&2; tail -n 15 "$D/sup.log" >&2 2>/dev/null; exit 1; }
t0=$(date +%s)

# 1) control surface through the simulated resource manager
"$B/relmw_supervisor" --mount /dev/relmw_e2e --sched other --period-us 1000 --episodes 0 --library "$D/lib.txt" >"$D/sup.log" 2>&1 &
PID=$!
i=0; while [ ! -S "$QNXSIM_ROOT/dev/relmw_e2e/ctl" ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$QNXSIM_ROOT/dev/relmw_e2e/ctl" ] || fail "resource manager did not attach /dev/relmw_e2e"
$CTL status >"$D/status" || fail "relmwctl status failed"
head -n 1 "$D/status" | grep -q 'QNX API simulation on Linux (not QNX)' || fail "status does not carry the simulation label"
$CTL fault percep_bias | grep -qx 'OK fault percep_bias' || fail "fault command"
$CTL plan push_B,push_A | grep -qx 'OK plan push_B,push_A' || fail "plan command"
$CTL plan push_C >/dev/null && fail "invalid plan accepted"
i=0; while ! $CTL status | grep -qx 'plan=push_B,push_A' && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
$CTL status | grep -qx 'plan=push_B,push_A' || fail "status does not show the plan"
i=0; while ! $CTL events 64 | grep -q 'SUBTASK_START push_B' && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
$CTL events 64 | grep -q 'SUBTASK_START push_B' || fail "no SUBTASK_START push_B event"
$CTL quit | grep -qx 'OK quit' || fail "quit command"
i=0; while kill -0 "$PID" 2>/dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
kill -0 "$PID" 2>/dev/null && fail "supervisor did not exit after quit"
wait "$PID"; rc=$?; PID=
[ $rc -eq 0 ] || fail "supervisor exit code $rc"
[ -S "$QNXSIM_ROOT/dev/relmw_e2e/ctl" ] && fail "resmgr path still attached after exit"
"$B/qnxsim_slog2info" -b relmw >"$D/slog" || fail "qnxsim_slog2info"
head -n 1 "$D/slog" | grep -q 'QNX API simulation on Linux, not QNX' || fail "slog2info label"
grep -q 'resource manager up at /dev/relmw_e2e' "$D/slog" || fail "slog2 buffer lacks the resmgr line"
echo "test_sim_e2e: part 1 OK (resmgr /dev/relmw_e2e via relmwctl, slog2)"

# 2) robot driver as a separate process over the simulated /dev/robot resmgr: remote == in-process, crash recovery
RELMW_PROBE_PREFIX=$QNXSIM_ROOT sh "$S/tests/test_robotd_e2e.sh" "$B/relmw_robotd" "$B/relmw_supervisor" || fail "robotd e2e over the simulation"
echo "test_sim_e2e: OK in $(( $(date +%s) - t0 )) s (QNX API simulation on Linux, not QNX)"
