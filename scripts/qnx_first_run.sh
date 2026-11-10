#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
# First run on QNX 8.0, one command, after the QNX Everywhere license + SDP 8.0 are installed:
#
#   QNX_SDP=~/qnx800 scripts/qnx_first_run.sh            # build -> image -> boot QEMU -> run everything -> collect
#   scripts/qnx_first_run.sh --preflight                 # only check prerequisites (safe to run any time)
#   scripts/qnx_first_run.sh --keep-vm                   # leave the VM running afterwards (for recording the video)
#
# Steps (each logged to results/qnx_qemu_x86_64/first_run_<UTC timestamp>/NN_*.log):
#   0 preflight   SDP env script, qcc/q++, mkqnximage, qemu-system-x86_64, ssh/scp, KVM (optional)
#   1 static      scripts/qnx_syntax_check.sh (host stubs) — must stay green; secpolcompile check of
#                 config/secpol/relmw_secpol.txt (host tool from the SDP; -m, output discarded) + tools/secpol_lint.py
#   2 build       cmake + q++ for x86_64 (scripts/build_qnx.sh); `file` on every binary
#   3 image       mkqnximage --type=qemu --arch=x86_64 in ./qnx-vm (first time only), boot headless, --getip
#   4 deploy      scp binaries + scripts/target/relmw_target_run.sh to /data/relmw on the target
#   5 run         sh relmw_target_run.sh on the target (unit tests, N=100 bench, 1 kHz RT runs, resmgr demo, slog2, HAM)
#   6 collect     scp /data/relmw/out back; compare bench output with the Linux host bit-for-bit; regenerate dashboard
#   7 stop        mkqnximage --stop (unless --keep-vm)
# Exit codes: 0 all good, 3 prerequisites missing (nothing was changed), 1 a later step failed (see SUMMARY.txt).
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
ARCH=x86_64
VM_DIR="${VM_DIR:-$ROOT/qnx-vm}"
KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
MODE=full; KEEP_VM=0
for a in "$@"; do
  case "$a" in
    --preflight) MODE=preflight ;;
    --keep-vm) KEEP_VM=1 ;;
    -h|--help) sed -n 2,20p "$0"; exit 0 ;;
    *) echo "unknown option $a"; exit 2 ;;
  esac
done
TS="$(date -u +%Y%m%dT%H%M%SZ)"
RUN="$ROOT/results/qnx_qemu_x86_64/first_run_$TS"
[ "$MODE" = preflight ] && RUN="$(mktemp -d)"
mkdir -p "$RUN"
SUMMARY="$RUN/SUMMARY.txt"
FAILED=0
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$SUMMARY"; }
run_step() {  # run_step NN_name cmd...
  local name="$1"; shift
  log "=== $name: $*"
  if "$@" > "$RUN/$name.log" 2>&1; then log "PASS $name"; return 0; fi
  local rc=$?; log "FAIL $name (rc=$rc) — last lines:"; tail -15 "$RUN/$name.log" | sed 's/^/    /' | tee -a "$SUMMARY"
  FAILED=1; return $rc
}

# ---- 0 preflight -------------------------------------------------------------------------------------------
missing=()
[ -f "$QNX_SDP/qnxsdp-env.sh" ] || missing+=("QNX SDP 8.0 not found: $QNX_SDP/qnxsdp-env.sh (install SDP 8.0 with QNX Software Center, or set QNX_SDP=)")
command -v qemu-system-x86_64 >/dev/null || missing+=("qemu-system-x86_64 not installed")
command -v ssh >/dev/null && command -v scp >/dev/null || missing+=("ssh/scp not installed")
command -v cmake >/dev/null || missing+=("cmake not installed")
if [ -f "$QNX_SDP/qnxsdp-env.sh" ]; then
  # shellcheck disable=SC1091
  source "$QNX_SDP/qnxsdp-env.sh" >/dev/null
  command -v qcc >/dev/null || missing+=("qcc not on PATH after sourcing qnxsdp-env.sh")
  command -v q++ >/dev/null || missing+=("q++ not on PATH after sourcing qnxsdp-env.sh")
  command -v mkqnximage >/dev/null || missing+=("mkqnximage not on PATH (install the mkqnximage / QEMU quickstart package in QSC)")
  [ -f "$QNX_TARGET/usr/include/ha/ham.h" ] || missing+=("ha/ham.h missing in \$QNX_TARGET (install the High Availability Manager package in QSC)")
  [ -f "$QNX_TARGET/usr/include/sys/slog2.h" ] || missing+=("sys/slog2.h missing in \$QNX_TARGET")
fi
{ echo "QNX_SDP=$QNX_SDP"; uname -a; qemu-system-x86_64 --version 2>/dev/null | head -1; cmake --version | head -1
  [ -e /dev/kvm ] && echo "KVM: available" || echo "KVM: not available (QEMU falls back to TCG; slower, latency numbers are emulator numbers)"
  command -v qcc >/dev/null && qcc -V 2>&1 | head -5
  # C8 needs >= 2 vCPUs in the VM; record which mkqnximage option sets that (the 8.0 doc page defers to --help/--options)
  command -v mkqnximage >/dev/null && { mkqnximage --help 2>&1; mkqnximage --options 2>&1; } | grep -iE 'cpu|smp' | head -10
  command -v secpolcompile >/dev/null && echo "secpolcompile: $(command -v secpolcompile)"; } > "$RUN/00_preflight.log" 2>&1
if [ ${#missing[@]} -gt 0 ]; then
  log "PREFLIGHT: prerequisites missing — nothing was built or changed:"
  for m in "${missing[@]}"; do log "  - $m"; done
  exit 3
fi
log "PREFLIGHT: all prerequisites present ($(grep KVM "$RUN/00_preflight.log"))"
[ "$MODE" = preflight ] && exit 0

# ---- 1 static + 2 build --------------------------------------------------------------------------------------
run_step 01_static scripts/qnx_syntax_check.sh
run_step 01b_secpol_lint python3 tools/secpol_lint.py config/secpol/relmw_secpol.txt --repo .
if command -v secpolcompile >/dev/null; then
  # Draft policy (docs/secpol.md): the extern types slogger2_t / ham_t must exist in the image's policy; compiling the
  # file alone shows whether our part is valid 8.0 policy language. Not pushed to the target in this run.
  run_step 01c_secpolcompile secpolcompile -m -o "$RUN/relmw_secpol.bin" config/secpol/relmw_secpol.txt
fi
run_step 02_build scripts/build_qnx.sh "$ARCH" || { log "build failed: fix and re-run (see 02_build.log)"; exit 1; }
B="$ROOT/build-qnx-$ARCH"
run_step 02b_file bash -c "file $B/relmw_* $B/test_* | tee /dev/stderr | grep -q ldqnx"  # QNX ELF interpreter is ldqnx-64.so.2

# ---- 3 image + boot --------------------------------------------------------------------------------------------
[ -f "$KEY.pub" ] || ssh-keygen -q -t ed25519 -N "" -f "$KEY"
mkdir -p "$VM_DIR"
pushd "$VM_DIR" >/dev/null || exit 1
if [ ! -d output ]; then
  run_step 03_image mkqnximage --type=qemu --arch="$ARCH" --hostname=relmw-qnx --ssh-ident="$KEY.pub" --build || exit 1
fi
run_step 03b_boot mkqnximage --run=-h || exit 1
IP="$(timeout 180 mkqnximage --getip 2>>"$RUN/03c_getip.log" | tail -1)"
popd >/dev/null
[ -n "$IP" ] || { log "FAIL 03c_getip: no IP after 180 s (console: $VM_DIR/output/qemu.out)"; exit 1; }
log "target IP: $IP"
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 "root@$IP")
SCP=(scp -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -q)
for i in $(seq 1 30); do "${SSH[@]}" true 2>/dev/null && break; sleep 2; done

# ---- 4 deploy + 5 run ------------------------------------------------------------------------------------------
run_step 04_deploy bash -c "$(printf '%q ' "${SSH[@]}") 'rm -rf /data/relmw/out; mkdir -p /data/relmw/bin' && \
  $(printf '%q ' "${SCP[@]}") $B/relmw_supervisor $B/relmw_bench $B/relmw_ham_setup $B/test_* root@$IP:/data/relmw/bin/ && \
  $(printf '%q ' "${SCP[@]}") scripts/target/relmw_target_run.sh root@$IP:/data/relmw/"
run_step 05_target_run "${SSH[@]}" "sh /data/relmw/relmw_target_run.sh /data/relmw"

# ---- 6 collect + compare --------------------------------------------------------------------------------------
run_step 06_collect "${SCP[@]}" -r "root@$IP:/data/relmw/out" "$RUN/target"
H="$ROOT/results/host_linux"; Q="$RUN/target/bench"
if [ -f "$Q/episodes.jsonl" ]; then
  if cmp -s "$H/episodes.jsonl" "$Q/episodes.jsonl"; then log "PASS 06b_determinism: episodes.jsonl identical to the Linux host (sha256 $(sha256sum "$Q/episodes.jsonl" | cut -c1-12))"
  else log "FAIL 06b_determinism: QNX episodes.jsonl differs from host"; diff <(head -c 4000 "$H/episodes.jsonl") <(head -c 4000 "$Q/episodes.jsonl") > "$RUN/06b_diff.txt"; FAILED=1; fi
else log "FAIL 06b_determinism: no bench output from target"; FAILED=1; fi
mkdir -p "$ROOT/results/qnx_qemu_x86_64/latest"
cp -r "$RUN/target/." "$ROOT/results/qnx_qemu_x86_64/latest/" 2>/dev/null
cp "$RUN/target/"rt_report_*.json "$ROOT/results/qnx_qemu_x86_64/" 2>/dev/null
[ -f tools/make_dashboard.py ] && run_step 06c_dashboard python3 tools/make_dashboard.py --host "$H" --qnx "$ROOT/results/qnx_qemu_x86_64" --out docs/dashboard.html

# ---- 7 stop ------------------------------------------------------------------------------------------------------
if [ "$KEEP_VM" = 0 ]; then (cd "$VM_DIR" && mkqnximage --stop) >> "$RUN/07_stop.log" 2>&1; log "VM stopped"; else log "VM left running at $IP (--keep-vm)"; fi
log "RESULT: $([ $FAILED = 0 ] && echo ALL PASS || echo 'SOME STEPS FAILED — see above')  logs: $RUN"
exit $FAILED
