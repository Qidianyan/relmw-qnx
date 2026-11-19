// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX API simulation on Linux, not QNX. See compat/qnx_sim/README.md.
// Entry points of the simulation that have no QNX counterpart, plus the renamed functions that would otherwise collide
// with glibc symbols of the same name but different semantics.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#ifdef __APPLE__
typedef void* timer_t;
struct itimerspec {
  struct timespec it_interval;
  struct timespec it_value;
};
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define QNXSIM_LABEL "QNX API simulation on Linux, not QNX"
// Root of the simulated pathname space: $QNXSIM_ROOT, else /tmp/qnxsim-<uid>. A resmgr path /dev/x lives at <root>/dev/x.
const char* qnxsim_root(void);
int qnxsim_open(const char* path, int oflag, ...);
struct sigevent;
int qnxsim_timer_create(clockid_t clock_id, struct sigevent* evp, timer_t* timerid);
int qnxsim_timer_settime(timer_t timerid, int flags, const struct itimerspec* value, struct itimerspec* ovalue);
int qnxsim_timer_delete(timer_t timerid);
#ifdef __cplusplus
}
#endif
