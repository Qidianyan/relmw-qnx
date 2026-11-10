// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// POSIX (Linux host) timing + scheduling + watchdog stubs.
//  * PeriodicTimer: absolute-deadline sleeps via clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME);
//    the first release lands one period after construction; a wake more than one period late is
//    absorbed by re-anchoring to the next future-or-current period boundary (no catch-up burst).
//  * set_thread_sched(): SCHED_FIFO / SCHED_RR / SCHED_OTHER through pthread_setschedparam.
//    SCHED_SPORADIC is a QNX extension and is rejected on this OS.
//  * set_thread_cluster(): CPU affinity of the calling thread, by the QNX cluster vocabulary. "_all" restores the
//    process's initial allowed mask (captured once; inside a container that is a subset of the machine's CPUs),
//    "_cpu-N" pins to one CPU that must be both configured and allowed by the process mask.
//  * watchdog_*(): no-ops (systemd or a shell loop plays the HAM role on the host).
#define _POSIX_C_SOURCE 200809L
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // pthread_setaffinity_np / pthread_getaffinity_np / CPU_*
#endif
#include "relmw/platform.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <time.h>
#include <unistd.h>

namespace relmw::platform {

std::int64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (std::int64_t)ts.tv_sec * 1000000000LL + (std::int64_t)ts.tv_nsec;
}

struct PeriodicTimer::Impl {
  std::atomic<bool> interrupted{false};
};

PeriodicTimer::PeriodicTimer(std::int64_t period_ns)
    : period_(period_ns), next_release_(now_ns() + period_ns), impl_(new Impl) {}

PeriodicTimer::~PeriodicTimer() { delete impl_; }

std::int64_t PeriodicTimer::wait_tick() {
  if (!impl_ || period_ <= 0) return -1;
  for (;;) {
    if (impl_->interrupted.load(std::memory_order_acquire)) return -1;
    const std::int64_t rel = next_release_;
    const std::int64_t now = now_ns();
    if (now < rel) {
      // Absolute deadline: a spurious wakeup (EINTR) just re-loops and re-checks the flag.
      struct timespec ts;
      ts.tv_sec = (time_t)(rel / 1000000000LL);
      ts.tv_nsec = (long)(rel % 1000000000LL);
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
      continue;
    }
    const std::int64_t late = now - rel;  // >= 0: only reached once the release is due
    if (late > period_) {
      // More than one period late (e.g. the process was descheduled): absorb the missed
      // releases by re-anchoring to the next future-or-current period boundary instead of
      // firing a burst of catch-up ticks.
      while (next_release_ < now) next_release_ += period_;
    } else {
      next_release_ = rel + period_;
    }
    return late;
  }
}

void PeriodicTimer::interrupt() {
  if (impl_) impl_->interrupted.store(true, std::memory_order_release);
}

bool set_thread_sched(const std::string& policy, int priority, int low_priority,
                      std::int64_t budget_ns, std::int64_t period_ns, std::string* err) {
  (void)low_priority;  // only meaningful for SCHED_SPORADIC (not available here)
  (void)budget_ns;
  (void)period_ns;
  int pol = SCHED_OTHER;
  int prio = 0;
  if (policy == "fifo" || policy == "rr") {
    pol = (policy == "fifo") ? SCHED_FIFO : SCHED_RR;
    prio = priority;
  } else if (policy == "other") {
    pol = SCHED_OTHER;
    prio = 0;
  } else if (policy == "sporadic") {
    if (err) *err = "SCHED_SPORADIC not supported on this OS";
    return false;
  } else {
    if (err) *err = "unknown scheduling policy: " + policy;
    return false;
  }
  struct sched_param sp;
  memset(&sp, 0, sizeof sp);
  sp.sched_priority = prio;
  const int rc = pthread_setschedparam(pthread_self(), pol, &sp);
  if (rc != 0) {  // EPERM etc.: report, never crash
    if (err) *err = std::string("pthread_setschedparam: ") + strerror(rc);
    return false;
  }
  return true;
}

// ---- CPU affinity (clusters) -------------------------------------------------------------
namespace {

// The process's initial allowed mask, captured once on first use (sched_getaffinity of pid 0): inside a container
// this can be a strict subset of the machine's CPUs, and "_all" restores exactly this mask.
const cpu_set_t& allowed_mask() {
  static cpu_set_t m;
  static bool once = false;
  if (!once) {
    CPU_ZERO(&m);
    if (sched_getaffinity(0, sizeof m, &m) != 0) CPU_ZERO(&m);  // empty on error: everything but _all fails
    once = true;
  }
  return m;
}

}  // namespace

bool set_thread_cluster(const std::string& name, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = "EINVAL " + why;
    return false;
  };
  if (name == "_all") {
    const cpu_set_t& m = allowed_mask();
    if (pthread_setaffinity_np(pthread_self(), sizeof m, &m) != 0) return fail("cannot restore the initial mask");
    return true;
  }
  const std::string pre = "_cpu-";
  if (name.size() <= pre.size() || name.compare(0, pre.size(), pre) != 0)
    return fail("cluster name must be \"_all\" or \"_cpu-N\" (custom clusters exist only on QNX, "
                "defined by the startup program -c)");
  const std::string digits = name.substr(pre.size());
  for (char c : digits)
    if (c < '0' || c > '9')
      return fail("cluster name \"" + name + "\": N must be decimal digits only "
                  "(custom clusters exist only on QNX)");
  long n = 0;
  for (char c : digits) {
    n = n * 10 + (c - '0');
    if (n > 1024) return fail("cpu index out of range in cluster name \"" + name + "\"");
  }
  const long nconf = sysconf(_SC_NPROCESSORS_CONF);
  if (nconf < 0 || n >= nconf)
    return fail("cpu " + std::to_string(n) + " >= configured CPUs (" + std::to_string(nconf < 0 ? 0 : nconf) + ")");
  const cpu_set_t& m = allowed_mask();
  if (!CPU_ISSET((int)n, &m))
    return fail("cpu " + std::to_string(n) + " is not allowed by the process affinity mask");
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET((int)n, &one);
  if (pthread_setaffinity_np(pthread_self(), sizeof one, &one) != 0)
    return fail("pthread_setaffinity_np(cpu " + std::to_string(n) + ") failed");
  return true;
}

// "cpus=<comma list of CPU ids> mask=0x<hex>", the same format as the QNX implementation (qnx_timer.cpp).
std::string thread_affinity_text() {
  cpu_set_t s;
  CPU_ZERO(&s);
  const int rc = pthread_getaffinity_np(pthread_self(), sizeof s, &s);
  if (rc != 0) return std::string("cpus=? mask=? (pthread_getaffinity_np: ") + strerror(rc) + ")";
  std::string cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &s)) { if (!cpus.empty()) cpus += ','; cpus += std::to_string(cpu); }
  std::string hex;  // whole runmask, most significant nibble first, leading zeros trimmed
  for (int nib = (CPU_SETSIZE + 3) / 4 - 1; nib >= 0; --nib) {
    int v = 0;
    for (int b = 0; b < 4; ++b) {
      const int cpu = nib * 4 + b;
      if (cpu < CPU_SETSIZE && CPU_ISSET(cpu, &s)) v |= 1 << b;
    }
    if (!hex.empty() || v != 0 || nib == 0) hex += (char)("0123456789abcdef"[v]);
  }
  return "cpus=" + (cpus.empty() ? std::string("-") : cpus) + " mask=0x" + hex;
}

bool watchdog_attach(const char*, std::int64_t, int, int, std::string*) { return true; }

void watchdog_heartbeat() {}

}  // namespace relmw::platform
