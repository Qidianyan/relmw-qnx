// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX API simulation on Linux, not QNX. See compat/qnx_sim/README.md.
#pragma once
#include_next <sys/neutrino.h>
#ifdef __cplusplus
extern "C" {
#endif
// Not used by the relmw sources (they go through dispatch_block), provided for tests and the codelab.
rcvid_t MsgReceive(int chid, void* msg, size_t bytes, struct _msg_info* info);
#define _PULSE_TYPE 0
#ifdef __cplusplus
}
#endif
