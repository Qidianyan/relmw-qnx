#pragma once
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/siginfo.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef long rcvid_t;
struct _msg_info { uint32_t nd, srcnd; pid_t pid; int32_t tid, chid, scoid, coid; int16_t priority, flags;
                   size_t msglen, srcmsglen, dstmsglen; uint32_t reserved[4]; };
struct _pulse { uint16_t type; uint16_t subtype; int8_t code; uint8_t zero[3]; union sigval value; int32_t scoid; };
typedef struct iovec iov_t;
#define _PULSE_CODE_MINAVAIL 0
#define _PULSE_CODE_MAXAVAIL 127
#define _NTO_CHF_FIXED_PRIORITY 0x0001u
#define _NTO_CHF_UNBLOCK 0x0002u
#define _NTO_CHF_THREAD_DEATH 0x0004u
#define _NTO_CHF_DISCONNECT 0x0008u
#define _NTO_CHF_PRIVATE 0x0080u
#define _NTO_SIDE_CHANNEL 0x40000000u
#define SYSMGR_COID _NTO_SIDE_CHANNEL
#define _NTO_REGEVENT_ALLOW_ANY_SERVER (-1)
int ChannelCreate(unsigned flags);
int ChannelDestroy(int chid);
int ConnectAttach(uint32_t reserved, pid_t pid, int chid, unsigned index, int flags);
int ConnectDetach(int coid);
int MsgReceivePulse(int chid, void* pulse, size_t bytes, struct _msg_info* info);
int MsgSendPulse(int coid, int priority, int code, long value);
int MsgRegisterEvent(struct sigevent* event, int coid);
int MsgUnregisterEvent(const struct sigevent* event);
#ifdef __cplusplus
}
#endif
