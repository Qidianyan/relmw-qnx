// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// CPU-affinity ("cluster") tests: strict name parsing, "_all" restores the initial mask, and the fact that on the
// POSIX host a thread created AFTER set_thread_cluster inherits the new mask while one created BEFORE keeps its own
// (Linux threads inherit affinity; on QNX the inherit mask differs — the QNX implementation is the reviewer's).
// The affinity API is POSIX-only, so those checks compile on the host only.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // sched_getcpu / pthread_{get,set}affinity_np / CPU_*
#endif
#include "relmw/platform.hpp"
#include "minitest.hpp"

#ifndef __QNX__

#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <unistd.h>

using namespace relmw::platform;

static void self_mask(cpu_set_t* out) {
  CPU_ZERO(out);
  pthread_getaffinity_np(pthread_self(), sizeof *out, out);
}

static long nconf() { return sysconf(_SC_NPROCESSORS_CONF); }

TEST(affinity_cpu0_pins_and_reports) {
  std::string err;
  CHECK(set_thread_cluster("_cpu-0", &err));
  int on_cpu0 = 0;
  for (int i = 0; i < 1000; ++i)
    { sched_yield(); if (sched_getcpu() == 0) ++on_cpu0; }
  CHECK(on_cpu0 == 1000);
  const std::string t = thread_affinity_text();
  CHECK(t == "cpus=0 mask=0x1");
  CHECK(set_thread_cluster("_all", &err));   // leave the default mask for the following tests
}

TEST(affinity_all_restores_initial_mask) {
  cpu_set_t init, cur;
  self_mask(&init);
  std::string err;
  if (CPU_COUNT(&init) > 1) {
    CHECK(set_thread_cluster("_cpu-0", &err));
    self_mask(&cur);
    CHECK(!CPU_EQUAL(&init, &cur));
  }
  CHECK(set_thread_cluster("_all", &err));
  self_mask(&cur);
  CHECK(CPU_EQUAL(&init, &cur));
}

TEST(affinity_rejects_bad_names) {
  const char* bad[] = {"", "_cpu-", "_cpu--1", "_cpu-1x", "_cpu- 1", "big", "_cpu-99999"};
  std::string err;
  for (const char* b : bad) {
    err.clear();
    CHECK(!set_thread_cluster(b, &err));
    CHECK(!err.empty());
  }
  char over[32];
  std::snprintf(over, sizeof over, "_cpu-%ld", nconf());   // N == configured CPU count: out of range
  err.clear();
  CHECK(!set_thread_cluster(over, &err));
  CHECK(!err.empty());
  CHECK(set_thread_cluster("_all", &err));
}

TEST(affinity_inherited_by_new_threads) {
  cpu_set_t init;
  CPU_ZERO(&init);
  if (sched_getaffinity(0, sizeof init, &init) != 0) return;
  int first = -1, second = -1;
  for (int c = 0; c < CPU_SETSIZE; ++c) {
    if (!CPU_ISSET(c, &init)) continue;
    if (first < 0) first = c;
    else { second = c; break; }
  }
  if (second < 0) {   // only one allowed CPU: nothing to pin to
    std::printf("note: only %d allowed CPU(s); skipping the affinity inheritance test\n", CPU_COUNT(&init));
    return;
  }
  std::string err;
  std::atomic<bool> go{false};
  cpu_set_t before, early_mask, pinned, cur;
  self_mask(&before);
  std::thread early([&] {
    while (!go.load()) std::this_thread::yield();   // wait for the pin to happen
    self_mask(&early_mask);
  });
  CHECK(set_thread_cluster("_cpu-" + std::to_string(second), &err));
  self_mask(&pinned);
  CHECK(CPU_ISSET(second, &pinned));
  CHECK(!CPU_EQUAL(&before, &pinned));
  go.store(true);
  early.join();
  CHECK(CPU_EQUAL(&before, &early_mask));    // created BEFORE the call: keeps its old mask
  bool late_ok = false;
  std::thread late([&] { self_mask(&cur); late_ok = CPU_EQUAL(&pinned, &cur); });  // created AFTER: inherits it
  late.join();
  CHECK(late_ok);
  CHECK(set_thread_cluster("_all", &err));
}

#endif  // !__QNX__

TEST_MAIN()
