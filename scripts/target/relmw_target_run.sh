#!/bin/sh
# Runs ON the QNX 8.0 target (QEMU image built by mkqnximage). Uploaded and started by scripts/qnx_first_run.sh.
# Everything it produces goes to $OUT (default /data/relmw/out); the host copies that directory back.
# POSIX sh / QNX ksh only. Processes are stopped by PID only.
#   sh relmw_target_run.sh [/data/relmw]
D="${1:-/data/relmw}"
OUT="$D/out"
BIN="$D/bin"
mkdir -p "$OUT"
cd "$D" || exit 1
PASS=0; FAIL=0
note() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/target_run.log"; }
ok()   { PASS=$((PASS+1)); note "PASS $*"; }
bad()  { FAIL=$((FAIL+1)); note "FAIL $*"; }
step() { name="$1"; shift; note "--- $name: $*"; if "$@" > "$OUT/$name.txt" 2>&1; then ok "$name"; else bad "$name (rc=$?)"; fi; }
wait_file() { i=0; while [ ! -e "$1" ] && [ $i -lt "$2" ]; do sleep 1; i=$((i+1)); done; [ -e "$1" ]; }
stop_pid() { [ -n "$1" ] && kill "$1" 2>/dev/null; i=0; while kill -0 "$1" 2>/dev/null && [ $i -lt 10 ]; do sleep 1; i=$((i+1)); done; kill -9 "$1" 2>/dev/null; true; }

# 1. Platform evidence
step 00_uname uname -a
step 01_pidin_info pidin info
( command -v slay; command -v slog2info; command -v ham; command -v pidin; command -v on ) > "$OUT/02_tools.txt" 2>&1
SLAY="$(command -v slay)"; export RELMW_SLAY="$SLAY"
step 03_file_binaries sh -c "ls -l $BIN; command -v file >/dev/null && file $BIN/* || true"
step 04_slogger2_running sh -c "pidin -f an | grep -i slogger2"

# 2. Unit tests built with q++ (B5). test_posix_platform exercises the POSIX layer and is not built for QNX runs.
for t in "$BIN"/test_*; do
  [ -x "$t" ] || continue
  n=$(basename "$t"); [ "$n" = test_posix_platform ] && continue
  step "10_$n" "$t"
done

# 3. Deterministic benchmark: same N=100 seeds as the Linux host (B6: results must be bit-identical)
step 20_bench "$BIN/relmw_bench" --n 100 --out "$OUT/bench"

# 4. Real-time supervisor runs: 1 kHz, 30 episodes, sched other vs fifo vs sporadic, idle and under CPU load (B7)
run_sup() {  # $1=label $2=sched $3=load(0/1)
  LOADPIDS=""
  if [ "$3" = 1 ]; then
    for k in 1 2; do ( while :; do :; done ) & LOADPIDS="$LOADPIDS $!"; done   # 2 busy loops at default priority
  fi
  cp "$OUT/bench/failure_library.txt" "$OUT/lib_$1.txt"     # the supervisor rewrites its library; keep bench output pristine
  step "30_sup_$1" "$BIN/relmw_supervisor" --episodes 30 --period-us 1000 --sched "$2" --prio 40 \
        --mount "/dev/relmw_$1" --library "$OUT/lib_$1.txt" --report "$OUT/rt_report_$1.json"
  for p in $LOADPIDS; do stop_pid "$p"; done
}
run_sup other_idle other 0
run_sup fifo_idle fifo 0
run_sup other_load other 1
run_sup fifo_load fifo 1
run_sup sporadic_load sporadic 1

# 5. Resource manager demo (C1'/C2'): cat status, echo a fault, read the reply back
cp "$OUT/bench/failure_library.txt" "$OUT/lib_demo.txt"
"$BIN/relmw_supervisor" --episodes 0 --period-us 1000 --sched fifo --prio 40 --mount /dev/relmw \
    --library "$OUT/lib_demo.txt" > "$OUT/40_resmgr_supervisor.txt" 2>&1 &
SUP=$!
if wait_file /dev/relmw/status 10; then
  {
    echo '$ ls -l /dev/relmw'; ls -l /dev/relmw
    echo '$ cat /dev/relmw/status'; cat /dev/relmw/status
    echo '$ echo "fault percep_bias" > /dev/relmw/ctl'; echo "fault percep_bias" > /dev/relmw/ctl
    echo '$ cat /dev/relmw/ctl'; cat /dev/relmw/ctl
    sleep 5
    echo '$ cat /dev/relmw/events'; cat /dev/relmw/events
    echo '$ echo knock > /dev/relmw/ctl'; echo knock > /dev/relmw/ctl; cat /dev/relmw/ctl
    sleep 3
    echo '$ cat /dev/relmw/status'; cat /dev/relmw/status
  } > "$OUT/41_resmgr_demo.txt" 2>&1 && ok 41_resmgr_demo || bad 41_resmgr_demo
  ls /proc/mount/dev/relmw > "$OUT/42_proc_mount.txt" 2>&1
  echo quit > /dev/relmw/ctl
else
  bad "41_resmgr_demo (/dev/relmw/status did not appear)"
fi
stop_pid "$SUP"

# 6. slog2 evidence (C3')
step 50_slog2info sh -c "slog2info -b relmw | tail -200"

# 7. HAM: restart on death (C4'). Needs the ham manager running; start it if present and not running.
if command -v ham >/dev/null; then
  pidin -f an | grep -q '[ /]ham$' || ham
  sleep 1
  LIB="$OUT/lib_ham.txt"; cp "$OUT/bench/failure_library.txt" "$LIB"
  step 60_ham_setup "$BIN/relmw_ham_setup" "$BIN/relmw_supervisor --episodes 0 --period-us 1000 --sched fifo --prio 40 --mount /dev/relmw --library $LIB --ham"
  sleep 3
  P1=$(pidin -f an | awk '/relmw_supervisor/ {print $1; exit}')
  note "supervisor pid before kill: $P1"
  cat /dev/relmw/status > "$OUT/61_status_before_kill.txt" 2>&1
  if [ -n "$P1" ]; then
    "$SLAY" -f -9 -m pid "$P1"
    sleep 4
    P2=$(pidin -f an | awk '/relmw_supervisor/ {print $1; exit}')
    note "supervisor pid after kill: $P2"
    cat /dev/relmw/status > "$OUT/62_status_after_restart.txt" 2>&1
    if [ -n "$P2" ] && [ "$P2" != "$P1" ] && grep -q 'failure_library_entries=[1-9]' "$OUT/62_status_after_restart.txt"; then
      ok "63_ham_restart (pid $P1 -> $P2, library kept)"
    else bad "63_ham_restart (pid $P1 -> '$P2')"; fi
    # leave the system clean: detach is not needed for the demo VM, stop the restarted instance via its ctl
    echo quit > /dev/relmw/ctl 2>/dev/null
  else bad "63_ham_restart (supervisor not started by HAM)"; fi
else
  bad "60_ham (ham binary not on target; install the HAM package and add it to the image)"
fi

note "SUMMARY pass=$PASS fail=$FAIL"
[ "$FAIL" -eq 0 ]
