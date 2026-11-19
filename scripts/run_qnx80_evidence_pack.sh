#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
# One-click QNX SDP 8.0 QEMU automated build, real-time verification & forensic evidence packager.
#
# Usage:
#   QNX_SDP=~/qnx800 scripts/run_qnx80_evidence_pack.sh              # Full cross-build -> SMP QEMU -> RT/HAM/Shield/MLP -> tar.gz
#   scripts/run_qnx80_evidence_pack.sh --preflight                   # Non-destructive environment & toolchain audit
#   scripts/run_qnx80_evidence_pack.sh --host-verify                 # Run full host + qnx_sim + S+ evidence generation offline
#   QNX_SDP=~/qnx800 scripts/run_qnx80_evidence_pack.sh --keep-vm    # Leave QEMU VM running for video screen capture
#
# Evidence bundle produced under results/qnx_qemu_x86_64/evidence_pack_<UTC>/:
#   - 00_preflight.log          : SDP 8.0 env, qcc/q++, mkqnximage, KVM/SMP capability, secpolcompile
#   - 01_qnx_elf_headers.txt    : `file` proof of native QNX 8.0 x86_64 ELF (`ldqnx-64.so.2`)
#   - 02_secpol_compile.log     : `secpolcompile` binary policy compilation + `tools/secpol_lint.py`
#   - target/bench/             : Standard N=100 paired benchmark (`cmp` bit-identical to host_linux)
#   - target/bench_splus/       : S+ Tiny-MLP + 1 kHz Simplex SafetyShield N=100 medical_vial benchmark (0 intrusions)
#   - target/rt_report_*.json   : 5-way 1 kHz pulse release latency histograms (SCHED_OTHER/FIFO/SPORADIC + --cpu 1)
#   - target/kernel_trace.txt   : `tracelogger` + `traceprinter` microsecond pulse & MsgSend/MsgReply IPC trace
#   - target/91_sup_ham_robotd.txt : `slay -f -9` process crash -> HAM restart -> supervisor reconnect proof
#   - SHA256SUMS.txt            : Cryptographic manifest of every artifact + regenerated docs/dashboard.html
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
export TMPDIR=/tmp

QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
ARCH="${ARCH:-x86_64}"
VM_DIR="${VM_DIR:-$ROOT/qnx-vm}"
SMP_CPUS="${SMP_CPUS:-4}"
KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
MODE="full"
KEEP_VM=0

for a in "$@"; do
  case "$a" in
    --preflight)   MODE="preflight" ;;
    --host-verify) MODE="host-verify" ;;
    --keep-vm)     KEEP_VM=1 ;;
    -h|--help)     sed -n '2,23p' "$0"; exit 0 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

if [ "$MODE" = "host-verify" ]; then
  set -e
  echo "=== [run_qnx80_evidence_pack] Running full Host + QNX-Sim + S+ Evidence Verification ==="
  bash "$ROOT/scripts/build_direct.sh" all
  mkdir -p "$ROOT/results/host_splus"
  "$ROOT/build-host/relmw_bench" --n 100 --warmup 40 --bias 0.12 \
      --policy mlp --preset medical_vial --shield \
      --export-replay "$ROOT/results/host_splus/replay_showcase.bin" \
      --out "$ROOT/results/host_splus"
  python3 "$ROOT/tools/make_dashboard.py" \
      --host "$ROOT/results/host_linux" \
      --splus "$ROOT/results/host_splus" \
      --out "$ROOT/docs/dashboard.html"
  echo "=== [run_qnx80_evidence_pack] Host + S+ evidence verification complete ==="
  exit 0
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
PACK_DIR="$ROOT/results/qnx_qemu_x86_64/evidence_pack_$TS"
[ "$MODE" = "preflight" ] && PACK_DIR="$(mktemp -d)"
mkdir -p "$PACK_DIR"
SUMMARY="$PACK_DIR/SUMMARY.txt"
FAILED=0

log() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$SUMMARY"; }
run_step() {
  local name="$1"; shift
  log "=== $name: $*"
  if "$@" > "$PACK_DIR/$name.log" 2>&1; then
    log "PASS $name"
    return 0
  fi
  local rc=$?
  log "FAIL $name (rc=$rc)"
  tail -n 15 "$PACK_DIR/$name.log" | sed 's/^/    /' | tee -a "$SUMMARY"
  FAILED=1
  return $rc
}

# ---- 0. Preflight check -------------------------------------------------------------------------------------
missing=()
[ -f "$QNX_SDP/qnxsdp-env.sh" ] || missing+=("QNX SDP 8.0 not found at $QNX_SDP/qnxsdp-env.sh (register free QNX Everywhere license at qnx.software & install SDP 8.0)")
command -v qemu-system-x86_64 >/dev/null 2>&1 || missing+=("qemu-system-x86_64 not on PATH")
command -v ssh >/dev/null 2>&1 && command -v scp >/dev/null 2>&1 || missing+=("ssh/scp not on PATH")
command -v cmake >/dev/null 2>&1 || missing+=("cmake not on PATH")

if [ -f "$QNX_SDP/qnxsdp-env.sh" ]; then
  # shellcheck disable=SC1091
  source "$QNX_SDP/qnxsdp-env.sh" >/dev/null
  command -v qcc >/dev/null 2>&1 || missing+=("qcc missing after sourcing qnxsdp-env.sh")
  command -v q++ >/dev/null 2>&1 || missing+=("q++ missing after sourcing qnxsdp-env.sh")
  command -v mkqnximage >/dev/null 2>&1 || missing+=("mkqnximage missing (install com.qnx.qnx800.quickstart.qemu in QSC)")
  [ -f "${QNX_TARGET:-}/usr/include/ha/ham.h" ] || missing+=("ha/ham.h missing in QNX_TARGET (install High Availability Manager in QSC)")
  [ -f "${QNX_TARGET:-}/usr/include/sys/slog2.h" ] || missing+=("sys/slog2.h missing in QNX_TARGET")
fi

{
  echo "QNX_SDP=$QNX_SDP"
  echo "ARCH=$ARCH SMP_CPUS=$SMP_CPUS"
  uname -a
  [ -e /dev/kvm ] && echo "KVM: hardware virtualization enabled" || echo "KVM: unavailable (TCG emulation)"
  command -v qcc >/dev/null 2>&1 && qcc -V 2>&1 | head -n 5
} > "$PACK_DIR/00_preflight.log" 2>&1

if [ ${#missing[@]} -gt 0 ]; then
  log "PREFLIGHT BLOCKED — missing QNX SDP 8.0 / host prerequisites:"
  for m in "${missing[@]}"; do log "  * $m"; done
  [ "$MODE" = "preflight" ] && rm -rf "$PACK_DIR"
  exit 3
fi
log "PREFLIGHT OK ($(grep '^KVM:' "$PACK_DIR/00_preflight.log"))"
if [ "$MODE" = "preflight" ]; then
  rm -rf "$PACK_DIR"
  exit 0
fi

# ---- 1. Static checks & secpol compile ----------------------------------------------------------------------
run_step 01_qnx_syntax bash "$ROOT/scripts/qnx_syntax_check.sh"
run_step 02_secpol_lint python3 "$ROOT/tools/secpol_lint.py" "$ROOT/config/secpol/relmw_secpol.txt" --repo "$ROOT"
if command -v secpolcompile >/dev/null 2>&1; then
  run_step 02b_secpolcompile secpolcompile -m -o "$PACK_DIR/relmw_secpol.bin" "$ROOT/config/secpol/relmw_secpol.txt"
fi

# ---- 2. Cross-compile all C++17 targets with qcc/q++ (-Wall -Wextra -Werror) --------------------------------
B="$ROOT/build-qnx-$ARCH"
run_step 03_cross_build cmake -S "$ROOT" -B "$B" -G "${CMAKE_GENERATOR:-Ninja}" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/qnx-$ARCH.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror"
run_step 03b_cross_compile cmake --build "$B" --parallel
run_step 04_verify_qnx_elf bash -c "file $B/relmw_* $B/test_* | tee '$PACK_DIR/01_qnx_elf_headers.txt' | grep -q ldqnx"

# ---- 3. Build & boot multi-core QNX 8.0 QEMU image ----------------------------------------------------------
[ -f "$KEY.pub" ] || ssh-keygen -q -t ed25519 -N "" -f "$KEY"
mkdir -p "$VM_DIR"
pushd "$VM_DIR" >/dev/null || exit 1
if [ ! -d output ]; then
  run_step 05_mkqnximage mkqnximage --type=qemu --arch="$ARCH" --cpus="$SMP_CPUS" \
      --hostname=relmw-qnx80 --ssh-ident="$KEY.pub" --build || exit 1
fi
run_step 05b_boot_qemu mkqnximage --cpus="$SMP_CPUS" --run=-h || exit 1
IP="$(timeout 180 mkqnximage --getip 2>>"$PACK_DIR/05c_getip.log" | tail -n 1)"
popd >/dev/null || exit 1
[ -n "$IP" ] || { log "FAIL: QEMU target did not report IP within 180 s"; exit 1; }
log "QNX 8.0 QEMU target online at $IP"

SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 "root@$IP")
SCP=(scp -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -q)
for _i in $(seq 1 30); do "${SSH[@]}" true 2>/dev/null && break; sleep 2; done

# ---- 4. Deploy & run full QNX 8.0 target test + S+ benchmark + tracelogger ----------------------------------
run_step 06_deploy bash -c "$(printf '%q ' "${SSH[@]}") 'rm -rf /data/relmw/out && mkdir -p /data/relmw/bin' && \
  $(printf '%q ' "${SCP[@]}") $B/relmw_supervisor $B/relmw_bench $B/relmw_robotd $B/relmw_ham_setup $B/test_* root@$IP:/data/relmw/bin/ && \
  $(printf '%q ' "${SCP[@]}") '$ROOT/scripts/target/relmw_target_run.sh' root@$IP:/data/relmw/"

run_step 07_target_suite "${SSH[@]}" "sh /data/relmw/relmw_target_run.sh /data/relmw"
run_step 08_collect "${SCP[@]}" -r "root@$IP:/data/relmw/out" "$PACK_DIR/target"

# ---- 5. Bit-exact determinism check + Dashboard rebuild + SHA-256 tarball -----------------------------------
if [ -f "$PACK_DIR/target/bench/episodes.jsonl" ]; then
  if cmp -s "$ROOT/results/host_linux/episodes.jsonl" "$PACK_DIR/target/bench/episodes.jsonl"; then
    log "PASS 09_bit_exact: QNX 8.0 episodes.jsonl matches host_linux byte-for-byte"
  else
    log "FAIL 09_bit_exact: QNX 8.0 episodes.jsonl diverged from host_linux"
    FAILED=1
  fi
fi

mkdir -p "$ROOT/results/qnx_qemu_x86_64"
cp "$PACK_DIR/target/"rt_report_*.json "$ROOT/results/qnx_qemu_x86_64/" 2>/dev/null || true
SPLUS_ARGS=()
[ -f "$PACK_DIR/target/bench_splus/summary.json" ] && SPLUS_ARGS=(--splus "$PACK_DIR/target/bench_splus")
run_step 10_dashboard python3 "$ROOT/tools/make_dashboard.py" \
    --host "$ROOT/results/host_linux" \
    --qnx "$ROOT/results/qnx_qemu_x86_64" \
    "${SPLUS_ARGS[@]}" \
    --out "$ROOT/docs/dashboard.html"

(
  cd "$PACK_DIR" || exit 1
  if command -v sha256sum >/dev/null 2>&1; then
    find . -type f ! -name SHA256SUMS.txt -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS.txt
  else
    find . -type f ! -name SHA256SUMS.txt -print0 | sort -z | xargs -0 shasum -a 256 > SHA256SUMS.txt
  fi
)
BUNDLE="$ROOT/results/qnx_qemu_x86_64/qnx80_evidence_bundle_${TS}.tar.gz"
tar -czf "$BUNDLE" -C "$(dirname "$PACK_DIR")" "$(basename "$PACK_DIR")"
log "EVIDENCE TARBALL READY: $BUNDLE"

if [ "$KEEP_VM" -eq 0 ]; then
  (cd "$VM_DIR" && mkqnximage --stop) >> "$PACK_DIR/11_stop_vm.log" 2>&1 || true
fi
exit $FAILED
