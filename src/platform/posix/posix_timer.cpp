// POSIX (Linux host) timing + scheduling + watchdog stubs.
//  * PeriodicTimer: absolute-deadline sleeps via clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME);
//    the first release lands one period after construction; a wake more than one period late is
//    absorbed by re-anchoring to the next future-or-current period boundary (no catch-up burst).
//  * set_thread_sched(): SCHED_FIFO / SCHED_RR / SCHED_OTHER through pthread_setschedparam.
//    SCHED_SPORADIC is a QNX extension and is rejected on this OS.
//  * watchdog_*(): no-ops (systemd or a shell loop plays the HAM role on the host).
#define _POSIX_C_SOURCE 200809L
#include "relmw/platform.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <time.h>

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

bool watchdog_attach(const char*, std::int64_t, int, int, std::string*) { return true; }

void watchdog_heartbeat() {}

}  // namespace relmw::platform
