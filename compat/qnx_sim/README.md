# compat/qnx_sim — QNX API simulation on Linux, not QNX

> **QNX API simulation on Linux, not QNX.** Nothing produced with this layer is a QNX measurement.
> It shows that relmw's QNX-specific source files (`src/platform/qnx/*`, `apps/relmw_robotd.cpp`) compile,
> link and behave correctly *as programs*. It does not show anything about QNX timing, scheduling,
> priority inheritance, security policies or the QNX kernel itself. Real QNX numbers come only from the
> QNX 8.0 target runs in `results/qnx/` (placeholders until the target run).

## What it is

A small user-space library (`src/qnx_sim.cpp`, one translation unit) plus wrapper headers (`include/`) that
implement the subset of the QNX 8.0 API used by relmw, on top of Linux AF_UNIX `SOCK_SEQPACKET` sockets:

| QNX API used by relmw | How the simulation does it |
| --- | --- |
| `ChannelCreate` / `ChannelDestroy` | a listening abstract-namespace socket per channel |
| `ConnectAttach` / `ConnectDetach` | a connected socket; `coid` = its fd (same process only) |
| `MsgSend` / `MsgReceive` / `MsgReply` / `MsgError` | one framed record per message; `rcvid` = the server-side fd |
| `MsgSendPulse` / `MsgReceivePulse` / `struct _pulse` | a pulse record (code, value); messages arriving during `MsgReceivePulse` are queued for the next `MsgReceive` |
| `SIGEV_PULSE_INIT`, `MsgRegisterEvent`, `timer_create/settime/delete` (`CLOCK_MONOTONIC`) | a timer thread on `steady_clock` that sends a pulse per expiry; `MsgRegisterEvent` accepts and ignores the event |
| `resmgr_attach` / `resmgr_detach`, `dispatch_*`, `iofunc_*` (read/write verify, `_IO_READ`, `_IO_WRITE`, `_IO_MSG`, `_RESMGR_NPARTS`, `_RESMGR_NOREPLY`) | a filesystem socket at `$QNXSIM_ROOT<path>`; `open()` of that path from a simulated program connects to it (an `_IO_CONNECT` record carrying the open mode), and an OCB with `ioflag` is kept per connection |
| `slog2_register`, `slog2f` / `vslog2f` | appends text lines to `$QNXSIM_ROOT/slog2/<buffer set>.log` (verbosity filter applied); `qnxsim_slog2info` prints them |
| `pthread_cluster_*`, `ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET)` | Linux CPU affinity |
| `ham_*` | prints "HAM not simulated" and returns success (no process restart) |

`QNXSIM_ROOT` defaults to `/tmp/qnxsim-<uid>`. Tools: `qnxsim_io read PATH` / `qnxsim_io write PATH TEXT`
(what `cat` / `echo >` do on QNX) and `qnxsim_slog2info` (first line: `# QNX API simulation on Linux, not QNX (slog2 subset)`).

## Build and test

```sh
cmake -S code -B build_sim -DRELMW_QNX_SIM=ON
cmake --build build_sim -j
ctest --test-dir build_sim          # 14 tests, incl. test_qnx_sim, test_sim_e2e, test_sim_bench_identical
```

`RELMW_QNX_SIM=ON` defines `__QNX__` and `RELMW_QNX_SIM=1` globally, so the **same `#ifdef __QNX__` sources**
that a QNX build compiles are compiled here. It is refused together with a real QNX toolchain. The binaries
announce themselves as `QNX API simulation on Linux (not QNX)` in `status`, in slog2 and on stdout.

* `test_qnx_sim` — 7 unit tests of the simulated calls (round trip, concurrent clients, pulses, timer grid, resmgr
  read/write/open-mode/`_IO_MSG`/fallthrough, slog2, clusters).
* `test_sim_e2e` — the QNX-path supervisor serves `/dev/relmw_e2e` through the simulated resource manager;
  `scripts/relmwctl` (with `RELMW_QNX_SIM=1`) drives status/fault/plan/events/quit; then `relmw_robotd` serves
  `/dev/robot…` as a separate process: remote run == in-process run (counters and learned library byte-identical),
  and a robotd killed 3 times mid-run is reconnected and the episode re-run. About 4 s.
* `test_sim_bench_identical` — `relmw_bench` built this way writes results byte-identical to `results/host_linux/`
  (the benchmark core is platform-independent; the platform layer only drives timing).

## Semantic differences from QNX 8.0 (read before quoting any number)

| Area | QNX 8.0 (official docs) | This simulation |
| --- | --- | --- |
| Blocking states | `MsgSend()` puts the sender SEND-blocked, then REPLY-blocked; the kernel copies data directly between address spaces ([MsgSend](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgsend.html)) | the sender blocks in `recv()` on a socket; data is copied twice through the Linux kernel. No SEND/REPLY states are visible to any tool |
| Priority inheritance | the receiving thread runs at the sender's priority; pulses carry a priority ([Priority inheritance and messages](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/ipc_Priority_inheritance_messages.html)) | **not simulated.** Priorities in `SIGEV_PULSE_INIT` / `MsgSendPulse` are stored and ignored |
| Receive order | messages queued in priority order, FIFO within a priority | `poll()` order over the open connections |
| `EINTR` in `MsgSend()` | a signal can interrupt SEND/REPLY-blocked senders | the simulation retries internally |
| Timers | `timer_create()` with a pulse event, kernel-driven expiry, timer tolerance / high-resolution options ([timer_create](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html)) | a `std::thread` sleeping on `steady_clock`; latency is Linux user-space latency under whatever load the host has. **Latency numbers from a simulated run say nothing about QNX** |
| `SCHED_OTHER` with a priority | allowed on QNX | Linux rejects it; the supervisor logs a warning and stays at the default priority |
| Secure events | `MsgRegisterEvent()` registers the event with the kernel and the kernel validates delivery ([MsgRegisterEvent](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msgregisterevent.html)) | accepted and ignored; no validation |
| Resource manager | path registered in the process manager's namespace; `open()` of any process resolves it; `_IO_CONNECT` handled by `iofunc` defaults incl. permissions ([resmgr_attach](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/r/resmgr_attach.html)) | a socket file under `$QNXSIM_ROOT`; only programs built with this layer (or `qnxsim_io`) can open it; no permission check, no `stat`, no `_IO_CLOSE` handler, no union mounts (a second attach of a live path is `EBUSY`) |
| `_IO_*` values | system constants | internal values (`_IO_READ=0x0101`, …) used only inside the simulation |
| slog2 | shared-memory ring buffers read with `slog2info` ([slog2f](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/slog2f.html)) | an append-only text file per buffer set |
| HAM | restarts a dead process per its conditions/actions | **not simulated**; the HAM calls succeed and do nothing. Restart is exercised by the test script instead |
| Security policies (secpol), `procnto` abilities | enforced by the kernel | **not simulated** |
| CPU runmask / clusters | QNX scheduler clusters | Linux CPU affinity |
| Connections | any process can `ConnectAttach()` to a `pid,chid` | only inside the same process; cross-process only through resmgr paths |

## Implementation notes

* The wrapper headers rename `open()` → `qnxsim_open()` and `timer_*` → `qnxsim_timer_*` with function-like
  macros, and are put on the include path of the simulated translation units only. A side effect: in those files,
  `std::fstream::open` is renamed as well, so they use `fopen()` instead.
* The steady-state receive path (`MsgReceivePulse` on the timer) does not allocate, so the QNX code's
  `test_rt_noalloc` stays meaningful; the allocation numbers are still not QNX numbers.
* A disconnecting client's fd is closed only after its OCB was dropped, so a reused fd number never hits a stale OCB.
