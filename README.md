# relmw-qnx — a real-time reliability supervisor for robot manipulation on QNX 8.0

> **Status (2026-11-10): work in progress for the QNX Global Hackathon (Robotics & Medical).**
> The portable C++ core, host benchmark and tests run today on a Linux host. The QNX 8.0 layer
> (`src/platform/qnx/`) is written but **has not yet been built with qcc or run on a QNX target** —
> QEMU evidence will be added under `results/qnx_qemu_x86_64/` once the QNX SDP 8.0 toolchain is installed.
> Nothing in this README claims QNX results that are not in the repository.

## What it does
Learned robot policies fail in the long tail, and their failures are **correlated**: after a plain
rollback the robot sees almost the same situation and repeats the same mistake. `relmw` wraps *any*
policy with:

1. **Task decomposition** into subtasks, each with an independent **verifier** (camera-style estimate,
   0.5 cm safety margin) and a settle/hold check.
2. **Failure classification** (lost contact, wrong-side stall, stopped short, off-axis drift,
   unrecoverable, regressed) and **correlation fingerprinting** (same mode + same end pose = repeat).
3. **Robot-only rollback** (the world is not reset) and **strategy selection from a failure library**
   (posterior P(success | subtask, mode, strategy) with an engineer's prior), never repeating a strategy
   that already failed, **escalating to a human** when the ladder is exhausted, and applying persistent
   fixes pro-actively to later subtasks (shared cause).
4. A **final postcondition re-check** (later subtasks can disturb earlier ones).

On QNX it runs as a **real-time supervisor process**:

| QNX 8.0 feature | Where |
|---|---|
| Kernel timer → **pulse** → `MsgReceivePulse` control loop on a private channel (`ChannelCreate(_NTO_CHF_PRIVATE)`, `MsgRegisterEvent`) | `src/platform/qnx/qnx_timer.cpp` `PeriodicTimer` |
| **Resource manager** `/dev/relmw/{status,events,ctl}` (`cat` the state, `echo` commands) | `src/platform/qnx/qnx_resmgr.cpp` |
| **slog2** event log (`slog2info -b relmw`) | `src/platform/qnx/qnx_log.cpp` |
| **SCHED_FIFO / SCHED_SPORADIC** control thread | `qnx_timer.cpp` `set_thread_sched()` |
| **High Availability Manager**: restart on death, heartbeat | `apps/relmw_ham_setup.cpp`, `qnx_timer.cpp` `watchdog_*` |
| Release-latency histogram (p50/p99/max, deadline misses) | `src/platform/latency.cpp`, `relmw_supervisor --report` |

## Results so far (Linux host, simulation only)
Deterministic C++ 2-D push simulator, scripted policy, N = 100 paired episodes per arm (same seeds),
failure library warmed on 40 held-out seeds and frozen. Field condition = per-episode injected fault
(bump 25 %, perception bias 25 %, slippery 20 %, none 30 %). Full output: `results/host_linux/`.

| condition | no middleware | naive retry | relmw (correlation-aware) |
|---|---|---|---|
| nominal | 100/100 | 100/100 | 100/100 |
| field | 78/100 | 80/100 | **98/100** (95 % CI 93.0–99.4 %; vs. none +20 pts, McNemar p = 1.9e-6) |

Perception-bias episodes: 0/21 without middleware, 1/21 with naive retry, 19/21 with relmw
(fixed by the `relocalize` strategy, then applied pro-actively to the next subtask).
The verifier had 0 false positives against ground truth. These are simulation numbers, not robot data.

The results are bit-identical between a GCC/libstdc++ build and a Clang/libc++ build (the simulator and
statistics use a self-contained xoshiro256** RNG, not `<random>` distributions).

## Build and run (Linux host)
```bash
scripts/build_host.sh                     # CMake + Ninja, then ctest
build-host/relmw_bench --n 100 --out results/host_linux
build-host/relmw_supervisor --episodes 20 --period-us 2000 --sched other --mount /tmp/relmw
#   in another terminal:  cat /tmp/relmw/status ; echo "fault percep_bias" > /tmp/relmw/ctl
```

## Build and run (QNX SDP 8.0, QEMU x86_64) — to be validated
```bash
QNX_SDP=~/qnx800 scripts/build_qnx.sh x86_64     # qcc/q++ via cmake/qnx-x86_64.cmake
QNX_SDP=~/qnx800 scripts/qemu_image.sh           # mkqnximage --type=qemu --arch=x86_64 --run
TARGET=root@<qemu-ip> scripts/deploy_qemu.sh     # copies binaries, runs bench + supervisor, fetches results
# on the target:
cat /dev/relmw/status
echo "fault percep_bias" > /dev/relmw/ctl
slog2info -b relmw
```
QNX version: QNX SDP 8.0 (QNX Everywhere non-commercial license). Target: QEMU x86_64 (aarch64le toolchain
file provided for Raspberry Pi 5). Toolchain: qcc/q++ (`gcc_ntox86_64`).

## Layout
See `docs/` (to come) and the header comments in `include/relmw/*.hpp`. The core (`src/core`) has no OS
calls and is shared by both platforms; QNX-specific code is isolated under `#ifdef __QNX__`.

## Provenance
The algorithm comes from our earlier Python/MuJoCo prototype (before 2026-11-02). Every file in this
repository was written during the hackathon submission period (first commit 2026-11-10).

## License
MIT (see `LICENSE`).
