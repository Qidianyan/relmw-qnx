#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/neutrino.h>
#ifdef __cplusplus
extern "C" {
#endif
struct _dispatch; typedef struct _dispatch dispatch_t;
typedef struct _resmgr_context {
  rcvid_t rcvid; struct _msg_info info; void* msg; dispatch_t* dpp; int id; void* extra;
  size_t msg_max_size; long status; size_t offset; size_t size; iov_t iov[1];
} resmgr_context_t;
typedef struct _resmgr_attr {
  unsigned flags; unsigned nparts_max; size_t msg_max_size; int (*other_func)(resmgr_context_t*, void* msg); unsigned reserved[4];
} resmgr_attr_t;
#define _RESMGR_NPARTS(n) (-(n))
#define _RESMGR_DETACH_ALL 0x0000u
#define _RESMGR_DETACH_PATHNAME 0x0001u
#define _RESMGR_FLAG_DIR 0x0100u
#define _RESMGR_CONNECT_NFUNCS 9u
#define _RESMGR_IO_NFUNCS 34u
#define SETIOV(_iop, _addr, _len) ((_iop)->iov_base = (void*)(_addr), (_iop)->iov_len = (_len))
ssize_t resmgr_msgget(resmgr_context_t* const ctp, void* msg, size_t size, size_t offset);
#ifdef __cplusplus
}
#endif
