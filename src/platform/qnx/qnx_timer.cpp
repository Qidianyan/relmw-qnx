// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX SDP 8.0 native timing + scheduling + HAM watchdog.
//  * Periodic tick = kernel timer (timer_create, CLOCK_MONOTONIC) delivering a PULSE to a private channel;
//    the control thread blocks in MsgReceivePulse() -> no signals, no polling, priority-inherited delivery.
//  * set_thread_sched(): SCHED_FIFO / SCHED_RR / SCHED_SPORADIC (budgeted real-time server).
//  * watchdog_*(): High Availability Manager self-attach + heartbeat (libham).
// API signatures checked against the QNX SDP 8.0 C Library Reference and HAM Developer's Guide (2026-11-10):
//   ChannelCreate, ConnectAttach, SIGEV_PULSE_INIT, MsgRegisterEvent, timer_create, MsgReceivePulse, MsgSendPulse,
//   nsec2timespec, sched_param (sched_ss_*), pthread_setschedparam, ham_attach_self, ham_condition, ham_action_*.
#ifdef __QNX__
#include "relmw/platform.hpp"
#include <errno.h>
#include <ha/ham.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>
#include <unistd.h>

namespace relmw::platform {

static constexpr int kPulseTick = _PULSE_CODE_MINAVAIL + 1;
static constexpr int kPulseStop = _PULSE_CODE_MINAVAIL + 2;

std::int64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (std::int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

struct PeriodicTimer::Impl {
  int chid = -1, coid = -1;
  timer_t timer{};
  bool ok = false;
};

PeriodicTimer::PeriodicTimer(std::int64_t period_ns) : period_(period_ns), impl_(new Impl) {
  impl_->chid = ChannelCreate(_NTO_CHF_PRIVATE);
  if (impl_->chid == -1) { log(Level::Error, "ChannelCreate: %s", strerror(errno)); return; }
  impl_->coid = ConnectAttach(0, 0, impl_->chid, _NTO_SIDE_CHANNEL, 0);
  if (impl_->coid == -1) { log(Level::Error, "ConnectAttach: %s", strerror(errno)); return; }
  // Pulse priority = the calling (control) thread's current priority. NOT SIGEV_PULSE_PRIO_INHERIT: per the 8.0
  // sigevent docs that makes the receiver run at the process's *initial* priority, which would silently drop a
  // SCHED_FIFO/SPORADIC control thread back to the default priority on every tick. Construct the timer AFTER
  // set_thread_sched() (apps/relmw_supervisor.cpp does).
  struct sched_param cur;
  int cur_policy = SCHED_OTHER;
  int prio = 10;
  if (pthread_getschedparam(pthread_self(), &cur_policy, &cur) == EOK) prio = cur.sched_priority;
  struct sigevent ev;
  SIGEV_PULSE_INIT(&ev, impl_->coid, prio, kPulseTick, 0);
  struct sigevent ev_plain = ev;          // MsgRegisterEvent() replaces ev with a handle; keep the original
  // Secure-event registration: only the process manager (SYSMGR_COID) may deliver this pulse. timer_create() does
  // not require a registered event, so fall back to the plain event if either step refuses the handle.
  bool registered = MsgRegisterEvent(&ev, SYSMGR_COID) != -1;
  if (!registered) log(Level::Warn, "MsgRegisterEvent: %s (using unregistered event)", strerror(errno));
  if (timer_create(CLOCK_MONOTONIC, &ev, &impl_->timer) == -1) {
    if (registered && timer_create(CLOCK_MONOTONIC, &ev_plain, &impl_->timer) != -1) {
      log(Level::Warn, "timer_create rejected the registered event; using the plain pulse event");
    } else {
      log(Level::Error, "timer_create: %s", strerror(errno)); return;
    }
  }
  log(Level::Info, "periodic timer: %lld ns, pulse priority %d, secure event %s", (long long)period_ns, prio,
      registered ? "yes" : "no");
  struct itimerspec its;
  memset(&its, 0, sizeof its);
  nsec2timespec(&its.it_value, (uint64_t)period_ns);
  nsec2timespec(&its.it_interval, (uint64_t)period_ns);
  next_release_ = now_ns() + period_ns;
  if (timer_settime(impl_->timer, 0, &its, nullptr) == -1) {
    log(Level::Error, "timer_settime: %s", strerror(errno)); return;
  }
  impl_->ok = true;
}

PeriodicTimer::~PeriodicTimer() {
  if (impl_->ok) timer_delete(impl_->timer);
  if (impl_->coid != -1) ConnectDetach(impl_->coid);
  if (impl_->chid != -1) ChannelDestroy(impl_->chid);
  delete impl_;
}

std::int64_t PeriodicTimer::wait_tick() {
  if (!impl_->ok) return -1;
  struct _pulse p;
  for (;;) {
    if (MsgReceivePulse(impl_->chid, &p, sizeof p, nullptr) == -1) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (p.code == kPulseStop) return -1;
    if (p.code != kPulseTick) continue;
    std::int64_t t = now_ns();
    std::int64_t late = t - next_release_;
    // Timer overruns (missed ticks) are absorbed: re-anchor the expected release to the period grid.
    next_release_ += period_;
    while (next_release_ <= t - period_) next_release_ += period_;
    return late < 0 ? 0 : late;
  }
}

void PeriodicTimer::interrupt() {
  if (impl_->coid != -1) MsgSendPulse(impl_->coid, -1, kPulseStop, 0);
}

bool set_thread_sched(const std::string& policy, int priority, int low_priority, std::int64_t budget_ns,
                      std::int64_t period_ns, std::string* err) {
  struct sched_param sp;
  memset(&sp, 0, sizeof sp);
  int pol = SCHED_OTHER;
  if (policy == "fifo") pol = SCHED_FIFO;
  else if (policy == "rr") pol = SCHED_RR;
  else if (policy == "sporadic") {
    // 8.0 sched_param rules: sched_priority > sched_ss_low_priority, init_budget < repl_period, max_repl < SS_REPL_MAX.
    if (low_priority >= priority || budget_ns <= 0 || budget_ns >= period_ns) {
      if (err) *err = "sporadic needs low_priority < priority and 0 < budget < period";
      return false;
    }
    pol = SCHED_SPORADIC;
    sp.sched_ss_low_priority = low_priority;
    nsec2timespec(&sp.sched_ss_init_budget, (uint64_t)budget_ns);
    nsec2timespec(&sp.sched_ss_repl_period, (uint64_t)period_ns);
    sp.sched_ss_max_repl = 4;
  }
  sp.sched_priority = priority;
  int rc = pthread_setschedparam(pthread_self(), pol, &sp);
  if (rc != EOK) { if (err) *err = strerror(rc); return false; }
  return true;
}

// Self-attached heartbeat entity. Its name gets a "_hb" suffix so it never collides (EEXIST) with the external
// restart-on-death entity that apps/relmw_ham_setup registers under the plain process name.
// A heartbeat alone triggers nothing: HAM only acts on conditions. CONDHBEATMISSEDLOW -> log line in HAM's activity
// log; CONDHBEATMISSEDHIGH -> execute RELMW_HANG_ACTION (default: SIGKILL our own pid via slay), which turns a hang into
// a death that the restart action of the external entity handles. ham_condition() needs an explicit ham_connect().
bool watchdog_attach(const char* name, std::int64_t heartbeat_ns, int missed_low, int missed_high, std::string* err) {
  std::string ename = std::string(name) + "_hb";
  ham_entity_t* e = ham_attach_self(ename.c_str(), (uint64_t)heartbeat_ns, missed_low, missed_high, 0);
  if (!e) { if (err) *err = std::string("ham_attach_self: ") + strerror(errno); return false; }
  if (ham_connect(0) == -1) { if (err) *err = std::string("ham_connect: ") + strerror(errno); return false; }
  ham_condition_t* lo = ham_condition(e, CONDHBEATMISSEDLOW, "hb_missed_low", 0);
  if (lo) ham_action_log(lo, "log_low", "relmw: heartbeat missed (low)", 1, 0, 0);
  ham_condition_t* hi = ham_condition(e, CONDHBEATMISSEDHIGH, "hb_missed_high", 0);
  // HAM needs the FULL path of the program. slay syntax (8.0 Utilities Reference): slay -f -9 -m pid <pid>.
  // scripts/qnx_first_run.sh exports RELMW_SLAY from `command -v slay` on the target.
  const char* act = getenv("RELMW_HANG_ACTION");
  const char* slay = getenv("RELMW_SLAY");
  std::string cmd = act && *act ? std::string(act)
                                : std::string(slay && *slay ? slay : "/system/bin/slay") + " -f -9 -m pid " +
                                      std::to_string((long)getpid());
  if (!hi || !ham_action_execute(hi, "kill_hung", cmd.c_str(), 0)) {
    if (err) *err = std::string("ham_condition/ham_action_execute: ") + strerror(errno);
    return false;
  }
  log(Level::Info, "HAM: %s heartbeat %lld ms, low=%d high=%d -> \"%s\"", ename.c_str(),
      (long long)(heartbeat_ns / 1000000), missed_low, missed_high, cmd.c_str());
  return true;
}

void watchdog_heartbeat() { ham_heartbeat(); }

}  // namespace relmw::platform
#endif  // __QNX__
