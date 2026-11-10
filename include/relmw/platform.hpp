// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// OS layer. Two implementations behind one interface:
//   src/platform/qnx/   : QNX SDP 8.0 native (ChannelCreate/timer_create pulses/MsgReceivePulse,
//                         resource manager /dev/relmw, slog2, SCHED_FIFO/SCHED_SPORADIC, HAM heartbeat)
//   src/platform/posix/ : Linux host (clock_nanosleep TIMER_ABSTIME, FIFO control file, stderr log)
// The portable core (src/core) never includes this header.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace relmw::platform {

std::int64_t now_ns();                       // CLOCK_MONOTONIC

// Periodic tick source for the control loop.
class PeriodicTimer {
 public:
  explicit PeriodicTimer(std::int64_t period_ns);
  ~PeriodicTimer();
  PeriodicTimer(const PeriodicTimer&) = delete;
  PeriodicTimer& operator=(const PeriodicTimer&) = delete;
  // Block until the next tick. Returns the lateness in ns (actual wake - scheduled release), >= 0
  // normally; returns -1 on error/shutdown.
  std::int64_t wait_tick();
  std::int64_t period_ns() const { return period_; }
  // Ask a blocked wait_tick() to return -1 (thread-safe; QNX: sends a pulse to the timer channel).
  void interrupt();
 private:
  std::int64_t period_;
  std::int64_t next_release_ = 0;
  struct Impl;
  Impl* impl_ = nullptr;
};

// Real-time scheduling for the calling thread. policy: "fifo", "rr", "sporadic", "other".
// sporadic: budget_ns of CPU every period_ns at `priority`, dropping to low_priority when exhausted.
bool set_thread_sched(const std::string& policy, int priority, int low_priority = 0,
                      std::int64_t budget_ns = 0, std::int64_t period_ns = 0, std::string* err = nullptr);

// Structured logging (QNX: slog2 buffer "relmw"; POSIX: stderr with monotonic timestamp).
enum class Level { Debug, Info, Warn, Error };
void log_init(const char* buffer_name);
void log(Level lvl, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// Control surface. QNX: resource manager mounting /dev/relmw/{status,events,ctl}
// (`cat /dev/relmw/status`, `echo "fault percep_bias" > /dev/relmw/ctl`).
// POSIX: <dir>/status and <dir>/events files rewritten atomically, <dir>/ctl named FIFO.
struct ControlHooks {
  std::function<std::string()> status;           // text for the status node
  std::function<std::string()> events;           // text for the events node
  std::function<std::string(const std::string&)> command;   // handle one line written to ctl; returns reply
};
class ControlSurface {
 public:
  ControlSurface(std::string mount_point, ControlHooks hooks);   // e.g. "/dev/relmw" or "/tmp/relmw"
  ~ControlSurface();
  bool start(std::string* err = nullptr);        // spawns its own service thread
  void stop();
  void publish();                                // POSIX: refresh status/events files (no-op on QNX)
  // Accessors used by the OS-specific service code.
  std::string hooks_status() const { return hooks_.status ? hooks_.status() : std::string(); }
  std::string hooks_events() const { return hooks_.events ? hooks_.events() : std::string(); }
  std::string hooks_command(const std::string& l) const { return hooks_.command ? hooks_.command(l) : std::string("ERR no handler"); }
  struct Impl;
 private:
  std::string mount_;
  ControlHooks hooks_;
  Impl* impl_ = nullptr;
};

// Watchdog / supervision. QNX: self-attach to the High Availability Manager with a heartbeat
// (ham_attach_self + ham_heartbeat); HAM restarts the supervisor on death / missed heartbeats.
// POSIX: no-op (systemd or a shell loop plays that role on the host).
bool watchdog_attach(const char* name, std::int64_t heartbeat_ns, int missed_low, int missed_high, std::string* err = nullptr);
void watchdog_heartbeat();

}  // namespace relmw::platform
