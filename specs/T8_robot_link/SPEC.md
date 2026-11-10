# T8: robot driver as a separate process (checklist C6) — portable protocol + POSIX transport + relmw_robotd

Working directory: repository root (git repo, C++17, CMake+Ninja). Read first: include/relmw/{robot,sim2d,verifier,experiment,
middleware,platform}.hpp, src/core/{experiment,verifier,sim2d}.cpp, apps/relmw_supervisor.cpp, tests/test_rt_noalloc.cpp,
tests/test_supervisor_e2e.sh, CMakeLists.txt.
Do NOT touch src/platform/qnx/, tools/qnx_stubs/, scripts/qnx_*.sh. (The reviewer writes the QNX transport afterwards: a resource
manager /dev/robot0 that receives this same request struct as an `_IO_MSG` message and answers with MsgReply.)

## Goal
Today the supervisor owns the PushSim2D in-process. Split it: a driver process `relmw_robotd` owns the robot (the simulator) and
the "overhead camera" ground truth; the supervisor talks to it with fixed-size request/reply messages (one request -> one reply,
synchronous, like QNX MsgSend/MsgReceive/MsgReply). The in-process path must keep working and stay BIT-IDENTICAL:
`relmw_bench --n 100` output must be byte-identical to results/host_linux/{episodes.jsonl,summary.json} (CI does `cmp`).

## 1. Truth sensor interface (src/core, include/relmw)
- In robot.hpp add `class TruthSensor { public: virtual ~TruthSensor()=default; virtual Obs truth() = 0; };` (independent camera /
  ground truth; non-const because a remote implementation does IPC).
- PushSim2D also implements TruthSensor (`Obs truth() override { return true_state(); }`); keep `true_state() const` as is.
- CameraVerifier takes `TruthSensor*` instead of `const PushSim2D*` (same noise stream, same arithmetic).
- `make_task(TruthSensor& truth, std::uint64_t ep)` replaces the PushSim2D& version (PushSim2D converts implicitly, so callers
  compile unchanged). The oracle uses `oracle_verdict(truth.truth(), i)`.
- Nothing else in the core changes. Verify byte-identical bench output after this step BEFORE continuing.

## 2. Wire protocol `include/relmw/robot_proto.hpp` (header-only, portable, no OS headers)
- First 8 bytes of every request = a header laid out exactly like QNX `struct _io_msg`:
  `struct ProtoHdr { uint16_t type; uint16_t combine_len; uint16_t mgrid; uint16_t subtype; };`
  subtype = op code. Constants: `kRobotMgrId = 0x0100 + 0x52` (documented as "_IOMGR_PRIVATE_BASE + 0x52"; the QNX transport fills
  type/mgrid with the real QNX constants, portable code sets type=0, combine_len=sizeof(ProtoHdr), mgrid=kRobotMgrId).
- `enum class RobotOp : uint16_t { Hello=1, Reset, GetObs, Step, Pose, MoveTo, Truth, Knock, Status }`.
- `struct RobotRequest { ProtoHdr hdr; uint32_t version; uint32_t seq; int32_t i[4]; double d[4]; };`
- `struct RobotReply { int32_t status; uint32_t seq; double obs[2 + 2*kNumObjects]; double pose[2]; int64_t ticks; int32_t fault; int32_t pad; };`
  status 0 = OK, else an errno-style positive code (EINVAL bad op/args, EPROTO version mismatch, ENODEV no episode reset yet).
- `kRobotProtoVersion = 1`. static_assert both structs are trivially copyable and standard layout, and their sizes (fixed, no
  implicit padding surprises: check with offsetof where it matters). Helpers: `make_request(RobotOp, seq)`, `obs_to_wire`,
  `obs_from_wire` (memcpy of doubles: bit-exact).
- Ops: Hello(version) -> OK/EPROTO. Reset(i[0]=episode seed lo, i[1]=seed hi, i[2]=field_faults 0/1, i[3]=force_fault or -1)
  -> constructs a fresh PushSim2D(seed, cfg), reply.fault = (int)sim.fault(). GetObs -> obs. Step(d[0],d[1]) -> obs after step.
  Pose -> pose. MoveTo(d[0],d[1], i[0]=steps) -> robot pose after sim.move_robot_to. Truth -> obs = true_state.
  Knock(i[0]=obj, d[0],d[1]) -> OK. Status -> ticks + fault. Every reply carries ticks (sim.ticks()) and seq echoed.

## 3. Server and client (portable, src/core/robot_link.cpp + include/relmw/robot_link.hpp)
- `class RobotServer { public: void handle(const RobotRequest&, size_t req_len, RobotReply&); std::string status_text() const; }`
  owns `std::unique_ptr<PushSim2D>`; validates req_len >= sizeof(RobotRequest), hdr.mgrid, version, op range, obj index,
  finite doubles (std::isfinite) -> EINVAL otherwise. No allocation except in Reset. status_text(): one line
  `robot0 episode=<seed> fault=<name> ticks=<n> requests=<n> errors=<n>`.
- `class RobotTransport { public: virtual ~RobotTransport()=default; virtual int call(const RobotRequest&, RobotReply&) = 0; };`
  returns 0 or an errno value (transport failure).
- `class LoopbackTransport : public RobotTransport` (calls RobotServer::handle directly) — used by tests.
- `struct RobotLinkError : std::runtime_error { int code; }`.
- `class RemoteRobot : public RobotAdapter, public TruthSensor` over a `RobotTransport&`: hello() at construction (throws on
  mismatch), `Fault reset(uint64_t seed, const SimConfig&)`, get_obs/step/robot_pose/move_robot_to/truth/knock/ticks().
  Transport error or non-zero status -> throw RobotLinkError (the error path only; no allocation on the success path:
  extend tests/test_rt_noalloc.cpp with a case proving RemoteRobot::step over LoopbackTransport makes 0 allocations).
  Increment seq per call and check the reply echoes it (mismatch -> RobotLinkError EPROTO).

## 4. POSIX transport (src/platform/posix/posix_robot_link.cpp, declared in include/relmw/robot_link_os.hpp)
- `std::unique_ptr<RobotTransport> robot_connect(const std::string& path, std::string* err);`
- `int robot_serve(const std::string& path, RobotServer&, std::atomic<bool>& quit, std::string* err);`
- Host: AF_UNIX SOCK_SEQPACKET (message boundaries, like QNX messages). One client at a time is enough; serve loop uses poll()
  with 100 ms timeout so `quit` is honoured; unlink stale socket on start, unlink on exit. Client: send request, recv reply,
  check sizes; EPIPE/ECONNRESET/short read -> return the errno (no SIGPIPE: use MSG_NOSIGNAL). Reply timeout 2 s (poll).
- Keep the OS-neutral header free of POSIX/QNX includes so the QNX implementation can provide the same two functions.

## 5. `apps/relmw_robotd.cpp`
`relmw_robotd [--path P] [--quit-after-requests N]` (default path: /tmp/relmw_robot0.sock on POSIX; the reviewer will add
/dev/robot0 for QNX). Logs with pf::log. SIGINT/SIGTERM -> clean exit (unlink socket). `--quit-after-requests N` = exit(3) after
N requests (crash simulation for tests). Build it in CMake on host (QNX build: the reviewer wires the QNX source).

## 6. Supervisor (`apps/relmw_supervisor.cpp`)
- New flag `--robot PATH`: use a RemoteRobot via robot_connect(PATH) instead of an in-process PushSim2D. Without the flag the
  behaviour and output are unchanged.
- Generalise RealtimeRobot to wrap a `RobotAdapter&` (in-process PushSim2D or RemoteRobot), plus a knock callback and a ticks
  callback (the event log line prints ticks). Truth for make_task and for the end-of-episode ground-truth score comes from the
  same TruthSensor (PushSim2D or RemoteRobot).
- RobotLinkError during an episode: log Error "robot link lost: <code>", push event "ROBOT LINK LOST", count it in a new
  status field `robot_link_errors=N`, do not count the episode, then reconnect (retry every 200 ms, heartbeat still beating,
  honour quit) and re-run the SAME episode seed from the start (the robot state was lost with the driver process; this is the
  "process-level recovery" layer, the task-level layer being rollback/retry). Status line `robot=<in-process|PATH>`.
- Same seeds + same arm => same counters local vs remote (episodes/success/recovered/failures_detected/escalated/library entries).

## 7. Tests (register in CMakeLists.txt, all must pass under ctest)
- `tests/test_robot_link.cpp`: struct sizes/layout; server rejects short message, wrong mgrid, wrong version, bad op, bad object,
  NaN target (each EINVAL/EPROTO, server state unchanged); ops before Reset -> ENODEV; seq echo; EQUIVALENCE: for seeds
  1000..1029, field faults on, each of the 3 arms (single pass / naive / corr-aware with a fresh CorrelationAwareRetry), run
  the middleware once with an in-process PushSim2D and once with RemoteRobot over LoopbackTransport (same policy, same seeds)
  -> identical EpisodeReport (success, attempts incl. strategies/modes/steps, abort_reason), identical true_state bits
  (memcmp), identical ticks.
- `tests/test_robotd_e2e.sh` (sh, register inside `if(NOT RELMW_QNX)` like test_supervisor_e2e): temp dir; start
  relmw_robotd on $D/r.sock (PID via $!, trap kills ONLY recorded PIDs, never pkill/killall); run
  `relmw_supervisor --episodes 6 --arm mw --sched other --period-us 300 --mount $D/m1 --robot $D/r.sock` and, separately,
  the same without --robot (mount $D/m2); the final status printouts must agree on the line with episodes=/success=/... and on
  failure_library_entries. Then crash test: start robotd with `--quit-after-requests 3000`, start supervisor --episodes 8
  --robot ..., restart robotd (loop in the test: when the PID exits, start a new one, max 3 restarts) and check the
  supervisor finishes with episodes=8, robot_link_errors>=1, exit 0, and the SAME counters as the in-process run with 8 episodes.
  < 90 s total.
- Existing tests unchanged and green; test_rt_noalloc gains the RemoteRobot case.

## Acceptance (exit 0, zero warnings with g++)
```
rm -rf /tmp/relmw_t8 && CC=gcc CXX=g++ cmake -S . -B /tmp/relmw_t8 -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build /tmp/relmw_t8 2>&1 | grep -iE "warning|error" ; ctest --test-dir /tmp/relmw_t8 --output-on-failure | tail -4
/tmp/relmw_t8/relmw_bench --n 100 --out /tmp/relmw_t8_bench && cmp /tmp/relmw_t8_bench/episodes.jsonl results/host_linux/episodes.jsonl && cmp /tmp/relmw_t8_bench/summary.json results/host_linux/summary.json && echo BITIDENTICAL
```
Run test_robotd_e2e 3 times in a row; must pass each time. Do not edit results/. Every new file starts with
`// SPDX-License-Identifier: MIT` and `// Copyright (c) 2026 elidot` (shell: `#` comments). No git commits (the reviewer commits).
Budget: 30 minutes. Write files first, then iterate.

## Final reply
Diff summary (files + one line each), test output tail, BITIDENTICAL line, e2e timings of the 3 runs, anything you could not do.
