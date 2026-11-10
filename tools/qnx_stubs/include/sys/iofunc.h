#pragma once
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resmgr.h>
#include <sys/iomsg.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _iofunc_attr {
  void* mount; void* mmap_list; void* lock_list; void* acl; union { void* lockobj; pthread_mutex_t lock; };
  uint32_t flags; uint16_t count, rcount, wcount, rlocks, wlocks, reserved; off_t nbytes; ino_t inode;
  uid_t uid; gid_t gid; time_t mtime, atime, ctime; mode_t mode; nlink_t nlink; dev_t rdev;
} iofunc_attr_t;
typedef struct _iofunc_ocb { iofunc_attr_t* attr; int32_t ioflag; off_t offset; uint16_t sflag; uint16_t flags; void* reserved; } iofunc_ocb_t;
#ifndef RESMGR_OCB_T
#define RESMGR_OCB_T iofunc_ocb_t
#endif
#ifndef RESMGR_HANDLE_T
#define RESMGR_HANDLE_T iofunc_attr_t
#endif
struct _io_read { uint16_t type; uint16_t combine_len; uint32_t nbytes; uint32_t xtype; uint32_t zero; };
typedef union { struct _io_read i; } io_read_t;
struct _io_write { uint16_t type; uint16_t combine_len; uint32_t nbytes; uint32_t xtype; uint32_t zero; };
typedef union { struct _io_write i; } io_write_t;
#define _IO_FLAG_RD 0x00000001
#define _IO_FLAG_WR 0x00000002
#define _IO_XTYPE_MASK 0x000000ffu
#define _IO_XTYPE_NONE 0x00000000u
#define _IO_READ_GET_NBYTES(msg) ((size_t)(msg)->i.nbytes)
#define _IO_WRITE_GET_NBYTES(msg) ((size_t)(msg)->i.nbytes)
#define _IO_SET_READ_NBYTES(ctp, n) ((ctp)->status = (long)(n))
#define _IO_SET_WRITE_NBYTES(ctp, n) ((ctp)->status = (long)(n))
#define IOFUNC_ATTR_CTIME 0x00000002u
#define IOFUNC_ATTR_MTIME 0x00000004u
#define IOFUNC_ATTR_ATIME 0x00000008u
#ifndef S_IFNAM
#define S_IFNAM 0050000
#endif
enum _file_type { _FTYPE_MATCHED = -1, _FTYPE_ALL = -1, _FTYPE_ANY = 0, _FTYPE_FILE };
typedef struct _resmgr_connect_funcs { unsigned nfuncs; void* fn[9]; } resmgr_connect_funcs_t;
#define __RMIO(name, msgt) int (*name)(resmgr_context_t* ctp, msgt* msg, RESMGR_OCB_T* ocb)
typedef struct _resmgr_io_funcs {
  unsigned nfuncs;
  __RMIO(read, io_read_t); __RMIO(write, io_write_t); __RMIO(close_ocb, void); __RMIO(stat, void); __RMIO(notify, void);
  __RMIO(devctl, void); __RMIO(unblock, void); __RMIO(pathconf, void); __RMIO(lseek, void); __RMIO(chmod, void);
  __RMIO(chown, void); __RMIO(utime, void); __RMIO(openfd, void); __RMIO(fdinfo, void); __RMIO(lock, void);
  __RMIO(space, void); __RMIO(shutdown, void); __RMIO(mmap, void); __RMIO(msg, io_msg_t); __RMIO(reserved, void);
  __RMIO(dup, void); __RMIO(close_dup, void); __RMIO(lock_ocb, void); __RMIO(unlock_ocb, void); __RMIO(sync, void);
  __RMIO(power, void); __RMIO(acl, void); __RMIO(pause, void); __RMIO(unpause, void);
  __RMIO(read64, io_read_t); __RMIO(write64, io_write_t); __RMIO(notify64, void); __RMIO(utime64, void); __RMIO(check_msg, void);
} resmgr_io_funcs_t;
#undef __RMIO
void iofunc_func_init(unsigned nconnect, resmgr_connect_funcs_t* connect, unsigned nio, resmgr_io_funcs_t* io);
struct _client_info;
void iofunc_attr_init_sized(iofunc_attr_t* attr, mode_t mode, iofunc_attr_t* dattr, struct _client_info* info, size_t size);
#define iofunc_attr_init(attr, mode, dattr, info) iofunc_attr_init_sized((attr), (mode), (dattr), (info), sizeof(*(attr)))
int iofunc_read_verify(resmgr_context_t* ctp, io_read_t* msg, iofunc_ocb_t* ocb, int* nonblock);
int iofunc_write_verify(resmgr_context_t* ctp, io_write_t* msg, iofunc_ocb_t* ocb, int* nonblock);
#ifdef __cplusplus
}
#endif
