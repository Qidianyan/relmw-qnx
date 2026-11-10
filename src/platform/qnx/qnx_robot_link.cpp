// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX SDP 8.0 robot driver link (checklist C6): relmw_robotd is a resource manager at /dev/robot0; the supervisor
// open()s it and sends each fixed-size RobotRequest as an out-of-band _IO_MSG message with MsgSend(); the driver answers
// with MsgReply(). `cat /dev/robot0` prints RobotServer::status_text().
//
// Checked against QNX SDP 8.0 docs (2026-11-10):
//   Writing a Resource Manager > "Handling out-of-band (_IO_MSG) messages" (struct _io_msg, mgrid in
//     _IOMGR_PRIVATE_BASE.._IOMGR_PRIVATE_MAX, io_funcs.msg has no default handler, resmgr_msgget, MsgReply +
//     _RESMGR_NOREPLY, ENOSYS for a foreign mgrid);
//   Getting Started > "User-defined message I/O function handler" / "Device control I/O function handler" (refuse
//     combine messages: ctp->rcvid == 0; validate lengths against ctp->info.srcmsglen - ctp->offset and dstmsglen);
//   System Security Guide > "Permission checking for message handler functions" (_IO_MSG bypasses
//     iofunc_*_verify(): check the open mode ourselves -> require _IO_FLAG_WR);
//   C Library Reference: MsgSend(int, const void*, size_t, void*, size_t) -> long, MsgReply(rcvid_t, long, const void*,
//     size_t), dispatch_create_channel(int chid, unsigned flags) (resmgr channels need _NTO_CHF_UNBLOCK |
//     _NTO_CHF_DISCONNECT), ChannelCreate(_NTO_CHF_INHERIT_RUNMASK): the receiving thread runs with the SENDER's
//     runmask (Programmer's Guide, "Client runmask inheritance") — together with QNX's default priority inheritance
//     the driver executes each step at the control thread's priority and on the control thread's CPU.
#ifdef __QNX__
#include "relmw/platform.hpp"
#include "relmw/robot_link_os.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <unistd.h>

namespace relmw {
namespace {

constexpr std::uint16_t kQnxRobotMgrId = _IOMGR_PRIVATE_BASE + 0x52;   // same offset as kRobotMgrId documents

class QnxRobotTransport : public RobotTransport {
 public:
  explicit QnxRobotTransport(int fd) : fd_(fd) {}
  ~QnxRobotTransport() override { if (fd_ != -1) close(fd_); }
  int call(const RobotRequest& req, RobotReply& reply) override {
    RobotRequest m = req;                       // stack copy; no allocation on the tick path
    m.hdr.type = _IO_MSG;
    m.hdr.combine_len = sizeof(m.hdr);
    m.hdr.mgrid = kQnxRobotMgrId;
    // The fd IS a connection id on QNX: MsgSend() goes straight to the driver's channel (SEND->REPLY blocked).
    if (MsgSend(fd_, &m, sizeof m, &reply, sizeof reply) == -1) return errno ? errno : EIO;
    return 0;
  }
 private:
  int fd_;
};

struct ServeCtx {
  RobotServer* server = nullptr;
  iofunc_attr_t attr;
};
ServeCtx g_ctx;   // one driver per process

int robot_io_msg(resmgr_context_t* ctp, io_msg_t* msg, RESMGR_OCB_T* ocb) {
  if (ctp->rcvid == 0) return ENOTSUP;                               // combine message: refuse (8.0 guidance)
  if (msg->i.mgrid != kQnxRobotMgrId) return ENOSYS;                 // not for us
  if (!(ocb->ioflag & _IO_FLAG_WR)) return EPERM;                    // _IO_MSG has no verify helper: check open mode
  if (ctp->info.srcmsglen < ctp->offset + sizeof(RobotRequest)) return EBADMSG;
  if (ctp->info.dstmsglen < sizeof(RobotReply)) return EMSGSIZE;
  RobotRequest req;
  if (resmgr_msgget(ctp, &req, sizeof req, 0) < (ssize_t)sizeof req) return EBADMSG;
  req.hdr.type = 0;                                                  // back to the portable header for RobotServer
  req.hdr.mgrid = kRobotMgrId;
  RobotReply rep{};
  g_ctx.server->handle(req, sizeof req, rep);                        // protocol errors travel in rep.status
  MsgReply(ctp->rcvid, EOK, &rep, sizeof rep);
  return _RESMGR_NOREPLY;
}

int robot_io_read(resmgr_context_t* ctp, io_read_t* msg, RESMGR_OCB_T* ocb) {
  int status = iofunc_read_verify(ctp, msg, ocb, nullptr);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  std::string text = g_ctx.server->status_text() + "\n";            // read side is not on the real-time path
  size_t off = (size_t)ocb->offset;
  size_t n = off < text.size() ? text.size() - off : 0;
  if (n > _IO_READ_GET_NBYTES(msg)) n = _IO_READ_GET_NBYTES(msg);
  if (n) MsgReply(ctp->rcvid, (long)n, text.data() + off, n); else MsgReply(ctp->rcvid, 0, nullptr, 0);
  ocb->offset += (off_t)n;
  return _RESMGR_NOREPLY;
}

}  // namespace

std::unique_ptr<RobotTransport> robot_connect(const std::string& path, std::string* err) {
  int fd = open(path.c_str(), O_RDWR);
  if (fd == -1) { if (err) *err = "open(" + path + "): " + strerror(errno); return nullptr; }
  return std::unique_ptr<RobotTransport>(new QnxRobotTransport(fd));
}

int robot_serve(const std::string& path, RobotServer& server, std::atomic<bool>& quit, std::string* err) {
  g_ctx.server = &server;
  int chid = ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT | _NTO_CHF_INHERIT_RUNMASK);
  if (chid == -1) { if (err) *err = std::string("ChannelCreate: ") + strerror(errno); return -1; }
  dispatch_t* dpp = dispatch_create_channel(chid, DISPATCH_FLAG_NOLOCK);   // static handler table
  if (!dpp) { if (err) *err = std::string("dispatch_create_channel: ") + strerror(errno); ChannelDestroy(chid); return -1; }
  static resmgr_connect_funcs_t connect_funcs;
  static resmgr_io_funcs_t io_funcs;
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
  io_funcs.msg = robot_io_msg;
  io_funcs.read = robot_io_read;
  io_funcs.read64 = robot_io_read;
  // 0660, owned by the driver's own uid/gid: only the supervisor's account (and root) may open it for writing.
  iofunc_attr_init(&g_ctx.attr, S_IFNAM | 0660, nullptr, nullptr);
  g_ctx.attr.uid = getuid();
  g_ctx.attr.gid = getgid();
  resmgr_attr_t rattr;
  std::memset(&rattr, 0, sizeof rattr);
  rattr.nparts_max = 1;
  rattr.msg_max_size = sizeof(RobotRequest) + 64;
  int id = resmgr_attach(dpp, &rattr, path.c_str(), _FTYPE_ANY, 0, &connect_funcs, &io_funcs, &g_ctx.attr);
  if (id == -1) {
    if (err) *err = "resmgr_attach(" + path + "): " + strerror(errno) + " (needs PROCMGR_AID_PATHSPACE, see docs/secpol.md)";
    dispatch_destroy(dpp);
    return -1;
  }
  dispatch_context_t* ctp = dispatch_context_alloc(dpp);
  if (!ctp) { if (err) *err = std::string("dispatch_context_alloc: ") + strerror(errno); resmgr_detach(dpp, id, _RESMGR_DETACH_ALL); dispatch_destroy(dpp); return -1; }
  platform::log(platform::Level::Info, "robot driver at %s (chid %d, _IO_MSG mgrid 0x%x)", path.c_str(), chid, kQnxRobotMgrId);
  int rc = 0;
  while (!quit.load()) {
    dispatch_context_t* got = dispatch_block(ctp);          // MsgReceive() underneath; SIGTERM -> EINTR -> re-check quit
    if (!got) {
      if (errno == EINTR) continue;
      if (err) *err = std::string("dispatch_block: ") + strerror(errno);
      rc = -1;
      break;
    }
    ctp = got;
    dispatch_handler(ctp);
  }
  resmgr_detach(dpp, id, _RESMGR_DETACH_ALL);
  dispatch_context_free(ctp);
  dispatch_destroy(dpp);
  return rc;
}

}  // namespace relmw
#endif  // __QNX__
