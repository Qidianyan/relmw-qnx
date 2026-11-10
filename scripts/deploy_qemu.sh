#!/usr/bin/env bash
# Copy the QNX build to the QEMU target and run the demo + benchmark there.
#   TARGET=root@<ip> scripts/deploy_qemu.sh
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET="${TARGET:?set TARGET=root@<qemu-ip> (see scripts/qemu_image.sh output)}"
B=build-qnx-x86_64
ssh "$TARGET" 'mkdir -p /data/relmw'
scp "$B/relmw_supervisor" "$B/relmw_bench" "$B/relmw_ham_setup" "$TARGET:/data/relmw/"
ssh "$TARGET" 'cd /data/relmw && uname -a && ./relmw_bench --n 100 --out /data/relmw/results | tail -8'
ssh "$TARGET" 'cd /data/relmw && ./relmw_supervisor --episodes 20 --period-us 1000 --sched fifo --prio 40 \
  --library /data/relmw/results/failure_library.txt --report /data/relmw/results/rt_report.json'
scp -r "$TARGET:/data/relmw/results" results/qnx_qemu_x86_64
echo "results copied to results/qnx_qemu_x86_64"
