// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Checklist B9: prove the real-time path does not allocate.
//
// This TU replaces the global replaceable allocation functions. Every call is counted in one
// static std::atomic, but only while the thread_local flag g_counting is true. RAII struct
// CountAllocs turns counting on for the current thread and reports the delta via n().
#include "minitest.hpp"
#include "relmw/experiment.hpp"
#include "relmw/latency.hpp"
#include "relmw/platform.hpp"
#include "relmw/policy.hpp"
#include "relmw/robot_link.hpp"
#include "relmw/sim2d.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
std::atomic<long> g_allocs{0};   // total counted allocation calls
thread_local bool g_counting = false;

void* counted_malloc(std::size_t n) {
  if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n ? n : 1);
  if (!p) {
    std::fprintf(stderr, "test_rt_noalloc: out of memory\n");
    std::abort();
  }
  return p;
}
}  // namespace

void* operator new(std::size_t n) { return counted_malloc(n); }
void* operator new[](std::size_t n) { return counted_malloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// Turns allocation counting on for the current thread; n() = allocations since construction.
struct CountAllocs {
  CountAllocs() : base_(g_allocs.load(std::memory_order_relaxed)) { g_counting = true; }
  ~CountAllocs() { g_counting = false; }
  long n() const { return g_allocs.load(std::memory_order_relaxed) - base_; }
 private:
  long base_;
};

using relmw::LatencyStats;
using relmw::PushSim2D;
using relmw::SimConfig;
using relmw::Vec2;
namespace platform = relmw::platform;

// 1. LatencyStats hot path: record() x100k (0..10 ms incl. > deadline) + queries -> 0 allocations.
TEST(rt_latency_record_no_alloc) {
  LatencyStats s;                        // constructor sizes the histogram once
  s.set_deadline(5'000'000);             // 5 ms deadline
  {
    CountAllocs ca;
    for (int i = 0; i < 100000; ++i) s.record(static_cast<std::int64_t>(i % 11) * 1'000'000LL);  // 0..10 ms
    CHECK(ca.n() == 0);
    CHECK(s.count() == 100000);
    volatile std::int64_t sink = 0;
    sink += s.quantile(0.5);
    sink += s.quantile(0.99);
    sink += s.quantile(0.999);
    sink += s.max();
    sink += s.min();
    sink += static_cast<std::int64_t>(s.mean());
    sink += static_cast<std::int64_t>(s.deadline_misses());
    CHECK(sink == sink);                 // volatile: keep the queries alive
    CHECK(ca.n() == 0);
  }
  CHECK(s.max() == 10'000'000);
  CHECK(s.min() == 0);
  CHECK(s.deadline_misses() > 0);        // values above the 5 ms deadline were seen
}

// 2. PushSim2D::step() x2000 after warm-up, nominal + all field faults -> 0 allocations.
TEST(rt_sim2d_step_no_alloc) {
  for (int fault = 0; fault <= 3; ++fault) {   // 0 = nominal, 1..3 = bump / percep_bias / slippery
    SimConfig cfg;
    cfg.force_fault = fault;
    PushSim2D sim(1000 + fault, cfg);
    for (int w = 0; w < 10; ++w) sim.step(Vec2{0.25, 0.20});   // warm-up (setup, not measured)
    CHECK(sim.ticks() == 10);
    {
      CountAllocs ca;
      for (int i = 0; i < 2000; ++i) sim.step(Vec2{0.25, 0.20});
      CHECK(ca.n() == 0);
    }
    CHECK(sim.ticks() == 2010);
  }
}

// 3. Observation accessors x2000 -> 0 allocations.
TEST(rt_sim2d_obs_no_alloc) {
  PushSim2D sim(42);
  for (int w = 0; w < 10; ++w) sim.step(Vec2{0.0, 0.0});
  {
    CountAllocs ca;
    volatile double sink = 0.0;
    for (int i = 0; i < 2000; ++i) {
      sink += sim.get_obs().pusher.x;
      sink += sim.true_state().pusher.x;
      sink += sim.robot_pose().x;
    }
    CHECK(ca.n() == 0);
  }
}

// 4. PeriodicTimer(1 ms)::wait_tick() x100 after the constructor (POSIX impl) -> 0 allocations.
TEST(rt_periodic_timer_wait_tick_no_alloc) {
  platform::PeriodicTimer timer(1'000'000LL);   // constructor allocates the Impl; not measured
  {
    CountAllocs ca;
    for (int i = 0; i < 100; ++i) CHECK(timer.wait_tick() >= 0);
    CHECK(ca.n() == 0);
  }
}

// 5. platform::now_ns() x10000 -> 0 allocations.
TEST(rt_now_ns_no_alloc) {
  platform::now_ns();
  {
    CountAllocs ca;
    std::int64_t prev = 0;
    for (int i = 0; i < 10000; ++i) {
      const std::int64_t t = platform::now_ns();
      CHECK(t > 0);
      prev = t;
    }
    CHECK(prev > 0);
    CHECK(ca.n() == 0);
  }
}

// 6. One full nominal ReliabilityMiddleware::run() episode: document the allocation count and the
//    per-sim-tick rate (printed with printf; allocations happen only at subtask/attempt
//    boundaries, not per tick, so the rate is << 1). Not asserted to be zero.
TEST(rt_episode_allocs_only_at_boundaries) {
  PushSim2D sim(7, SimConfig{});                            // nominal condition
  relmw::ScriptedPushPolicy pol;
  pol.new_episode(7);
  relmw::MiddlewareConfig mc;
  mc.max_retries = 3;
  mc.seed = 7;
  relmw::ReliabilityMiddleware mw(pol, sim, nullptr, mc, nullptr);
  std::vector<relmw::Subtask> task = relmw::make_task(sim, 7);   // setup allocations, not measured
  const long ticks0 = sim.ticks();
  long total = 0;
  long dticks = 0;
  {
    CountAllocs ca;
    relmw::EpisodeReport rep = mw.run(task);
    total = ca.n();
    dticks = sim.ticks() - ticks0;
    CHECK(rep.attempts.size() >= 1);
  }
  const double per_tick = dticks > 0 ? static_cast<double>(total) / static_cast<double>(dticks) : 0.0;
  std::printf("[rt_noalloc] one nominal ReliabilityMiddleware::run() episode: %ld allocations / %ld sim ticks = %.4f allocs per tick\n",
              total, dticks, per_tick);
  CHECK(total >= 0);
  CHECK(dticks > 0);
  CHECK(per_tick < 1.0);   // << 1: allocations only at subtask/attempt boundaries, never per tick
}

// 7. RemoteRobot::step over LoopbackTransport x2000 after warm-up (hello + reset outside the counted
//    region) -> 0 allocations: the success path is stack-only request/reply structs.
TEST(rt_remote_robot_step_no_alloc) {
  relmw::RobotServer server;
  relmw::LoopbackTransport transport(server);
  relmw::RemoteRobot robot(transport);
  relmw::SimConfig sc;
  robot.reset(7, sc);
  for (int w = 0; w < 10; ++w) robot.step(relmw::Vec2{0.25, 0.20});   // warm-up (setup, not measured)
  CHECK(robot.ticks() == 10);
  {
    CountAllocs ca;
    for (int i = 0; i < 2000; ++i) {
      relmw::Obs o = robot.step(relmw::Vec2{0.25, 0.20});
      volatile double sink = o.pusher.x + o.obj[0].x + o.obj[1].x;
      (void)sink;
    }
    CHECK(ca.n() == 0);
  }
  CHECK(robot.ticks() == 2010);
}

TEST_MAIN()
