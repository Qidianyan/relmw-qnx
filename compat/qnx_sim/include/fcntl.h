// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX API simulation on Linux, not QNX. See compat/qnx_sim/README.md.
// open() of a path attached by resmgr_attach() must reach the simulated resource manager: rename it for the
// translation units built against the simulation only (no global interposition of glibc's open()).
#pragma once
#include_next <fcntl.h>
#include "qnx_sim.h"
#define open(...) qnxsim_open(__VA_ARGS__)
