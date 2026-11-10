#!/usr/bin/env bash
# Cross-build for QNX SDP 8.0 with qcc/q++. Requires a QNX Everywhere (non-commercial) license + SDP install.
#   QNX_SDP=~/qnx800 scripts/build_qnx.sh [x86_64|aarch64le]
set -euo pipefail
cd "$(dirname "$0")/.."
ARCH="${1:-x86_64}"
QNX_SDP="${QNX_SDP:-$HOME/qnx800}"
# shellcheck disable=SC1091
source "$QNX_SDP/qnxsdp-env.sh"
cmake -S . -B "build-qnx-$ARCH" -DCMAKE_TOOLCHAIN_FILE="cmake/qnx-$ARCH.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "build-qnx-$ARCH" -j"$(nproc)"
file "build-qnx-$ARCH/relmw_supervisor" "build-qnx-$ARCH/relmw_bench" || true
