// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Clean-room minimal type signatures transcribed from public QNX SDP 8.0 documentation for host -fsyntax-only checks.
// Contains NO QNX SDK proprietary header code.
// QNX struct sched_param has the POSIX sporadic-server members; glibc/Darwin do not. Rename the host struct while its
// header is parsed, then declare the QNX 8.0 layout under the real name (pthread.h prototypes then use it).
#pragma once
#include <stdint.h>
#ifdef __APPLE__
#define sched_param __host_sched_param
#include <pthread/sched.h>
#include_next <sched.h>
#undef sched_param
#include <time.h>
#else
// glibc's bits/sched.h plays its own #define/#undef game on sched_param, so pre-include the one header that defines
// the struct under a different name; its include guard then stops glibc from defining it again.
#define sched_param __glibc_sched_param
#include <bits/types/struct_sched_param.h>
#undef sched_param
#include_next <sched.h>
#include <bits/types/struct_timespec.h>
#endif
struct sched_param {
  int32_t sched_priority;
  int32_t sched_curpriority;
  union {
    int32_t reserved[8];
    struct {
      int32_t __ss_low_priority;
      int32_t __ss_max_repl;
      struct timespec __ss_repl_period;
      struct timespec __ss_init_budget;
    } __ss;
  } __ss_un;
};
#define sched_ss_low_priority __ss_un.__ss.__ss_low_priority
#define sched_ss_max_repl __ss_un.__ss.__ss_max_repl
#define sched_ss_repl_period __ss_un.__ss.__ss_repl_period
#define sched_ss_init_budget __ss_un.__ss.__ss_init_budget
#ifndef SCHED_SPORADIC
#define SCHED_SPORADIC 4
#endif
