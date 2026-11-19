#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
# Direct zero-CMake compiler + test runner for POSIX host (build-host) and QNX API simulation (build-sim).
# Compiles every translation unit with -std=c++17 -O2 -Wall -Wextra -Werror (zero warnings tolerated).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
export TMPDIR=/tmp

CXX="${CXX:-clang++}"
CXXFLAGS=(-std=c++17 -O2 -Wall -Wextra -Werror)
LDFLAGS=(-pthread)

CORE_SRCS=(
  src/core/rng.cpp
  src/core/sim2d.cpp
  src/core/policy.cpp
  src/core/verifier.cpp
  src/core/strategies.cpp
  src/core/retry_policy.cpp
  src/core/middleware.cpp
  src/core/stats.cpp
  src/core/experiment.cpp
  src/core/robot_link.cpp
)
[ -f src/core/safety_shield.cpp ] && CORE_SRCS+=(src/core/safety_shield.cpp)
[ -f src/core/nn_policy.cpp ] && CORE_SRCS+=(src/core/nn_policy.cpp)
[ -f src/core/telemetry_bridge.cpp ] && CORE_SRCS+=(src/core/telemetry_bridge.cpp)

POSIX_SRCS=(
  src/platform/posix/posix_timer.cpp
  src/platform/posix/posix_log.cpp
  src/platform/posix/posix_ctl.cpp
  src/platform/posix/posix_robot_link.cpp
  src/platform/latency.cpp
)

QNX_SRCS=(
  src/platform/qnx/qnx_timer.cpp
  src/platform/qnx/qnx_log.cpp
  src/platform/qnx/qnx_resmgr.cpp
  src/platform/qnx/qnx_robot_link.cpp
  src/platform/latency.cpp
)

UNIT_TESTS=(
  test_rng
  test_stats
  test_sim2d
  test_middleware
  test_retry_policy
  test_latency
  test_posix_platform
  test_experiment
  test_rt_noalloc
  test_robot_link
  test_plan
  test_affinity
)
[ -f tests/test_safety_shield.cpp ] && UNIT_TESTS+=(test_safety_shield)
[ -f tests/test_nn_policy.cpp ] && UNIT_TESTS+=(test_nn_policy)
[ -f tests/test_telemetry_bridge.cpp ] && UNIT_TESTS+=(test_telemetry_bridge)

MODE="${1:-all}"   # host | sim | all

build_and_test_host() {
  local B="$ROOT/build-host"
  mkdir -p "$B/obj"
  echo "=== [HOST] Building with $CXX (${CXXFLAGS[*]}) -> $B ==="
  local core_objs=()
  for s in "${CORE_SRCS[@]}"; do
    local o="$B/obj/$(basename "$s" .cpp).o"
    "$CXX" "${CXXFLAGS[@]}" -Iinclude -c "$s" -o "$o"
    core_objs+=("$o")
  done
  local plat_objs=()
  for s in "${POSIX_SRCS[@]}"; do
    local o="$B/obj/$(basename "$s" .cpp).o"
    "$CXX" "${CXXFLAGS[@]}" -Iinclude -c "$s" -o "$o"
    plat_objs+=("$o")
  done
  ar rcs "$B/librelmw_core.a" "${core_objs[@]}"
  ar rcs "$B/librelmw_platform.a" "${plat_objs[@]}" "${core_objs[@]}"

  "$CXX" "${CXXFLAGS[@]}" -Iinclude apps/relmw_bench.cpp "$B/librelmw_core.a" "${LDFLAGS[@]}" -o "$B/relmw_bench"
  "$CXX" "${CXXFLAGS[@]}" -Iinclude apps/relmw_supervisor.cpp "$B/librelmw_platform.a" "${LDFLAGS[@]}" -o "$B/relmw_supervisor"
  "$CXX" "${CXXFLAGS[@]}" -Iinclude apps/relmw_robotd.cpp "$B/librelmw_platform.a" "${LDFLAGS[@]}" -o "$B/relmw_robotd"

  for t in "${UNIT_TESTS[@]}"; do
    "$CXX" "${CXXFLAGS[@]}" -Iinclude "tests/${t}.cpp" "$B/librelmw_platform.a" "${LDFLAGS[@]}" -o "$B/${t}"
  done

  echo "=== [HOST] Running all host tests ==="
  local pass=0
  for t in "${UNIT_TESTS[@]}"; do
    echo "--- [HOST TEST] $t"
    "$B/$t"
    pass=$((pass + 1))
  done
  echo "--- [HOST TEST] test_supervisor_e2e"
  sh "$ROOT/tests/test_supervisor_e2e.sh" "$B/relmw_supervisor" "$ROOT/scripts/relmwctl"
  pass=$((pass + 1))
  echo "--- [HOST TEST] test_robotd_e2e"
  sh "$ROOT/tests/test_robotd_e2e.sh" "$B/relmw_robotd" "$B/relmw_supervisor"
  pass=$((pass + 1))
  echo "--- [HOST TEST] test_qnx_syntax"
  CXX="$CXX" bash "$ROOT/scripts/qnx_syntax_check.sh"
  pass=$((pass + 1))
  echo "--- [HOST TEST] test_dashboard"
  python3 "$ROOT/tools/test_make_dashboard.py"
  pass=$((pass + 1))
  echo "--- [HOST TEST] test_llm_assist"
  python3 "$ROOT/tools/test_llm_assist.py"
  pass=$((pass + 1))
  echo "--- [HOST TEST] test_secpol_lint"
  python3 "$ROOT/tools/test_secpol_lint.py"
  pass=$((pass + 1))
  if [ -f "$ROOT/tools/test_ros2_bridge.py" ]; then
    echo "--- [HOST TEST] test_ros2_bridge"
    python3 "$ROOT/tools/test_ros2_bridge.py"
    pass=$((pass + 1))
  fi
  echo "=== [HOST] ALL $pass HOST TESTS PASSED (0 warnings) ==="
}

build_and_test_sim() {
  local B="$ROOT/build-sim"
  mkdir -p "$B/obj"
  echo "=== [QNX_SIM] Building with $CXX (${CXXFLAGS[*]} -D__QNX__ -DRELMW_QNX_SIM=1) -> $B ==="
  local sim_inc=(-Icompat/qnx_sim/include -Itools/qnx_stubs/include)
  local sim_defs=(-D__QNX__ -DRELMW_QNX_SIM=1)

  "$CXX" "${CXXFLAGS[@]}" "${sim_defs[@]}" "${sim_inc[@]}" -c compat/qnx_sim/src/qnx_sim.cpp -o "$B/obj/qnx_sim.o"
  ar rcs "$B/libqnx_sim.a" "$B/obj/qnx_sim.o"

  for tool in qnxsim_io qnxsim_slog2info; do
    "$CXX" "${CXXFLAGS[@]}" "${sim_defs[@]}" "${sim_inc[@]}" "compat/qnx_sim/tools/${tool}.cpp" "$B/libqnx_sim.a" "${LDFLAGS[@]}" -o "$B/${tool}"
  done

  local core_objs=()
  for s in "${CORE_SRCS[@]}"; do
    local o="$B/obj/core_$(basename "$s" .cpp).o"
    "$CXX" "${CXXFLAGS[@]}" "${sim_defs[@]}" -Iinclude -c "$s" -o "$o"
    core_objs+=("$o")
  done
  local qnx_objs=()
  for s in "${QNX_SRCS[@]}"; do
    local o="$B/obj/qnx_$(basename "$s" .cpp).o"
    "$CXX" "${CXXFLAGS[@]}" "${sim_defs[@]}" -Iinclude "${sim_inc[@]}" -c "$s" -o "$o"
    qnx_objs+=("$o")
  done
  ar rcs "$B/librelmw_core.a" "${core_objs[@]}"
  ar rcs "$B/librelmw_platform.a" "${qnx_objs[@]}" "${core_objs[@]}"

  "$CXX" "${CXXFLAGS[@]}" "${sim_defs[@]}" -Iinclude apps/relmw_bench.cpp "$B/librelmw_core.a" "${LDFLAGS[@]}" -o "$B/relmw_bench"
  "$CXX" "${CXXFLAGS[@]}" "${sim_defs[@]}" -Iinclude apps/relmw_supervisor.cpp "$B/librelmw_platform.a" "$B/libqnx_sim.a" "${LDFLAGS[@]}" -o "$B/relmw_supervisor"
  "$CXX" "${CXXFLAGS[@]}" "${sim_defs[@]}" -Iinclude apps/relmw_robotd.cpp "$B/librelmw_platform.a" "$B/libqnx_sim.a" "${LDFLAGS[@]}" -o "$B/relmw_robotd"

  local sim_tests=()
  for t in "${UNIT_TESTS[@]}"; do
    [ "$t" = "test_posix_platform" ] && continue
    "$CXX" "${CXXFLAGS[@]}" "${sim_defs[@]}" -Iinclude "tests/${t}.cpp" "$B/librelmw_platform.a" "$B/libqnx_sim.a" "${LDFLAGS[@]}" -o "$B/${t}"
    sim_tests+=("$t")
  done
  "$CXX" "${CXXFLAGS[@]}" "${sim_defs[@]}" "${sim_inc[@]}" tests/qnx_sim/test_qnx_sim.cpp "$B/libqnx_sim.a" "${LDFLAGS[@]}" -o "$B/test_qnx_sim"

  echo "=== [QNX_SIM] Running all QNX API simulation tests ==="
  local pass=0
  for t in "${sim_tests[@]}"; do
    echo "--- [QNX_SIM TEST] $t"
    "$B/$t"
    pass=$((pass + 1))
  done
  echo "--- [QNX_SIM TEST] test_qnx_sim"
  "$B/test_qnx_sim"
  pass=$((pass + 1))
  echo "--- [QNX_SIM TEST] test_sim_e2e"
  sh "$ROOT/tests/qnx_sim/test_sim_e2e.sh" "$B" "$ROOT"
  pass=$((pass + 1))
  echo "--- [QNX_SIM TEST] test_sim_bench_identical"
  mkdir -p "$B/sim_bench"
  "$B/relmw_bench" --n 100 --out "$B/sim_bench" >/dev/null
  cmp "$B/sim_bench/episodes.jsonl" "$ROOT/results/host_linux/episodes.jsonl"
  cmp "$B/sim_bench/summary.json" "$ROOT/results/host_linux/summary.json"
  pass=$((pass + 1))
  echo "=== [QNX_SIM] ALL $pass QNX_SIM TESTS PASSED (0 warnings) ==="
}

case "$MODE" in
  host) build_and_test_host ;;
  sim)  build_and_test_sim ;;
  all)  build_and_test_host; build_and_test_sim ;;
  *) echo "usage: $0 [host|sim|all]" >&2; exit 2 ;;
esac
