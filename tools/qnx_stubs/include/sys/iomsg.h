// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX 8.0 <sys/iomsg.h> subset: the _IO_MSG out-of-band message header ("Handling out-of-band (_IO_MSG) messages",
// Writing a Resource Manager; "User-defined message I/O function handler", Getting Started). Values are placeholders.
#pragma once
#include <stdint.h>
#define _IO_MSG 0x0113
#define _IOMGR_PRIVATE_BASE 0xf000
#define _IOMGR_PRIVATE_MAX 0xffff
struct _io_msg { uint16_t type; uint16_t combine_len; uint16_t mgrid; uint16_t subtype; };
typedef union { struct _io_msg i; } io_msg_t;
