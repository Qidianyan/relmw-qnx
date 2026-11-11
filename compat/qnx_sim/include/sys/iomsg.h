// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX API simulation on Linux, not QNX. See compat/qnx_sim/README.md.
// I/O message types the simulated dispatch layer routes. The values are internal to the simulation (the transcribed
// stub defines only _IO_MSG); a real client and server agree on them through the QNX headers.
#pragma once
#include_next <sys/iomsg.h>
#ifndef _IO_READ
#define _IO_READ 0x0101
#endif
#ifndef _IO_WRITE
#define _IO_WRITE 0x0102
#endif
#ifndef _IO_READ64
#define _IO_READ64 0x0128
#endif
#ifndef _IO_WRITE64
#define _IO_WRITE64 0x0129
#endif
