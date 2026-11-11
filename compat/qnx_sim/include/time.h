// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX API simulation on Linux, not QNX. See compat/qnx_sim/README.md.
// timer_create() with a SIGEV_PULSE sigevent: glibc would read sigev_notify == 4 as SIGEV_THREAD_ID, so the QNX
// semantics need their own implementation; renamed for simulation translation units only.
#pragma once
#include_next <time.h>
#include "qnx_sim.h"
#define timer_create qnxsim_timer_create
#define timer_settime qnxsim_timer_settime
#define timer_delete qnxsim_timer_delete
