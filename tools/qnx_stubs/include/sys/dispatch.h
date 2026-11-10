#pragma once
#include <sys/iofunc.h>
#include <sys/resmgr.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef union _dispatch_context { resmgr_context_t resmgr_context; } dispatch_context_t;
dispatch_t* dispatch_create(void);
dispatch_t* dispatch_create_channel(int chid, unsigned flags);
#define DISPATCH_FLAG_NOLOCK 0x00000001u
int dispatch_destroy(dispatch_t* dpp);
dispatch_context_t* dispatch_context_alloc(dispatch_t* dpp);
void dispatch_context_free(dispatch_context_t* ctp);
dispatch_context_t* dispatch_block(dispatch_context_t* ctp);
int dispatch_handler(dispatch_context_t* ctp);
int resmgr_attach(dispatch_t* dpp, resmgr_attr_t* attr, const char* path, enum _file_type file_type, unsigned flags,
                  const resmgr_connect_funcs_t* connect_funcs, const resmgr_io_funcs_t* io_funcs, RESMGR_HANDLE_T* handle);
int resmgr_detach(dispatch_t* dpp, int id, unsigned flags);
#ifdef __cplusplus
}
#endif
