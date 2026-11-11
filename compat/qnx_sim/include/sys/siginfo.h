// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX API simulation on Linux, not QNX. See compat/qnx_sim/README.md.
// The transcribed stub's SIGEV_PULSE_INIT drops coid/priority/code (it only has to compile). The simulation keeps them:
// coid in sigev_signo, priority and code in the padding of glibc's struct sigevent.
#pragma once
#include_next <sys/siginfo.h>
#undef SIGEV_PULSE_INIT
static inline void __qnxsim_sigev_pulse_init(struct sigevent* e, int coid, short priority, short code, void* value) {
  e->sigev_notify = SIGEV_PULSE;
  e->sigev_signo = coid;
  e->_sigev_un._pad[0] = priority;
  e->_sigev_un._pad[1] = code;
  e->sigev_value.sival_ptr = value;
}
#define SIGEV_PULSE_INIT(e, coid, prio, code, val) \
  __qnxsim_sigev_pulse_init((e), (coid), (short)(prio), (short)(code), (void*)(uintptr_t)(val))
