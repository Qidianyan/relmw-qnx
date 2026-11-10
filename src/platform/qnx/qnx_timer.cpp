// QNX SDP 8.0 native timing + scheduling + HAM watchdog.
//  * Periodic tick = kernel timer (timer_create, CLOCK_MONOTONIC) delivering a PULSE to a private channel;
//    the control thread blocks in MsgReceivePulse() -> no signals, no polling, priority-inherited delivery.
//  * set_thread_sched(): SCHED_FIFO / SCHED_RR / SCHED_SPORADIC (budgeted real-time server).
//  * watchdog_*(): High Availability Manager self-attach + heartbeat (libham).
#ifdef __QNX__
#include "relmw/platform.hpp"
#include <errno.h>
#include <ha/ham.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>

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
  struct sigevent ev;
  SIGEV_PULSE_INIT(&ev, impl_->coid, SIGEV_PULSE_PRIO_INHERIT, kPulseTick, 0);
  // Secure-event registration: only the process manager/kernel may deliver this pulse.
  if (MsgRegisterEvent(&ev, SYSMGR_COID) == -1)
    log(Level::Warn, "MsgRegisterEvent: %s (continuing with unregistered event)", strerror(errno));
  if (timer_create(CLOCK_MONOTONIC, &ev, &impl_->timer) == -1) {
    log(Level::Error, "timer_create: %s", strerror(errno)); return;
  }
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

bool watchdog_attach(const char* name, std::int64_t heartbeat_ns, int missed_low, int missed_high, std::string* err) {
  ham_entity_t* e = ham_attach_self(name, (uint64_t)heartbeat_ns, missed_low, missed_high, 0);
  if (!e) { if (err) *err = strerror(errno); return false; }
  return true;
}

void watchdog_heartbeat() { ham_heartbeat(); }

}  // namespace relmw::platform
#endif  // __QNX__
