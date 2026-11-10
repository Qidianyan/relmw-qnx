#pragma once
#include <signal.h>
#include <stdint.h>
#define SIGEV_PULSE 4
#define SIGEV_PULSE_PRIO_INHERIT (-1)
// SIGEV_PULSE_INIT(&event, coid, priority, code, value): value stored in sigev_value.sival_ptr
static inline void __qnx_stub_sigev_pulse_init(struct sigevent* e, int coid, short priority, short code, void* value) {
  (void)coid; (void)priority; (void)code;
  e->sigev_notify = SIGEV_PULSE;
  e->sigev_value.sival_ptr = value;
}
#define SIGEV_PULSE_INIT(e, coid, prio, code, val) \
  __qnx_stub_sigev_pulse_init((e), (coid), (short)(prio), (short)(code), (void*)(uintptr_t)(val))
