#pragma once
#include_next <time.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void nsec2timespec(struct timespec* timespec_p, uint64_t nsec);
#if defined(__APPLE__) && !defined(RELMW_QNX_SIM)
struct sigevent;
typedef void* timer_t;
struct itimerspec {
  struct timespec it_interval;
  struct timespec it_value;
};
int timer_create(clockid_t clock_id, struct sigevent* evp, timer_t* timerid);
int timer_settime(timer_t timerid, int flags, const struct itimerspec* value, struct itimerspec* ovalue);
int timer_delete(timer_t timerid);
#endif
#ifdef __cplusplus
}
#endif
