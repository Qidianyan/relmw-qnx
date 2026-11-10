// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX resource manager exposing the reliability supervisor as files under /dev/relmw:
//   /dev/relmw/status  (read)  : live counters, current subtask, control-loop latency p50/p99/max
//   /dev/relmw/events  (read)  : last N middleware events (VERIFY_FAIL, ROLLBACK, RETRY, ESCALATE ...)
//   /dev/relmw/ctl     (write) : one-line commands, e.g. `echo "fault percep_bias" > /dev/relmw/ctl`
// Any process (shell, HMI, a cell controller) talks to the supervisor through plain open/read/write,
// i.e. QNX native message passing (_IO_READ/_IO_WRITE) handled by this dispatch loop.
// Checked against QNX SDP 8.0 docs (2026-11-10): resmgr_attach, iofunc_func_init, iofunc_attr_init, dispatch_create,
// dispatch_context_alloc/block/handler, iofunc_read_verify/write_verify, _IO_READ_GET_NBYTES, resmgr_msgget.
// Needs PROCMGR_AID_PATHSPACE (root on the mkqnximage target).
#ifdef __QNX__
#include "relmw/platform.hpp"
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <sys/stat.h>

namespace relmw::platform {

struct ControlSurface::Impl {
  ControlSurface* self = nullptr;
  dispatch_t* dpp = nullptr;
  resmgr_connect_funcs_t connect_funcs;
  resmgr_io_funcs_t io_funcs;
  iofunc_attr_t attr_status, attr_events, attr_ctl;
  int id_status = -1, id_events = -1, id_ctl = -1;
  pthread_t thread{};
  volatile bool running = false;
  std::string snap_status, snap_events, last_reply;
};

static ControlSurface::Impl* g_impl = nullptr;   // one control surface per process

static int relmw_io_read(resmgr_context_t* ctp, io_read_t* msg, RESMGR_OCB_T* ocb) {
  int status = iofunc_read_verify(ctp, msg, ocb, nullptr);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  ControlSurface::Impl* im = g_impl;
  std::string* src = nullptr;
  if (ocb->attr == &im->attr_status) src = &im->snap_status;
  else if (ocb->attr == &im->attr_events) src = &im->snap_events;
  else src = &im->last_reply;
  // A fresh snapshot is taken when a reader starts at offset 0 (each `cat` sees a consistent view).
  if (ocb->offset == 0) {
    if (src == &im->snap_status && im->self) *src = im->self->hooks_status();
    else if (src == &im->snap_events && im->self) *src = im->self->hooks_events();
  }
  off_t off = ocb->offset;
  size_t len = src->size();
  size_t nleft = off < (off_t)len ? len - (size_t)off : 0;
  size_t nbytes = nleft < _IO_READ_GET_NBYTES(msg) ? nleft : _IO_READ_GET_NBYTES(msg);
  if (nbytes) {
    SETIOV(ctp->iov, src->data() + off, nbytes);
    _IO_SET_READ_NBYTES(ctp, nbytes);
    ocb->offset += nbytes;
    ocb->attr->flags |= IOFUNC_ATTR_ATIME;
    return _RESMGR_NPARTS(1);
  }
  _IO_SET_READ_NBYTES(ctp, 0);
  return _RESMGR_NPARTS(0);
}

static int relmw_io_write(resmgr_context_t* ctp, io_write_t* msg, RESMGR_OCB_T* ocb) {
  int status = iofunc_write_verify(ctp, msg, ocb, nullptr);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
  ControlSurface::Impl* im = g_impl;
  if (ocb->attr != &im->attr_ctl) return EROFS;
  size_t nbytes = _IO_WRITE_GET_NBYTES(msg);
  // 8.0 sample: reject writes that claim more data than the message carries.
  if (nbytes > (size_t)ctp->info.srcmsglen - (size_t)ctp->offset - sizeof(io_write_t)) return EBADMSG;
  char buf[512];
  size_t n = nbytes < sizeof buf - 1 ? nbytes : sizeof buf - 1;   // commands are one short line; longer input is truncated
  ssize_t got = resmgr_msgget(ctp, buf, n, sizeof(msg->i));
  if (got == -1) return errno;
  buf[got] = '\0';
  std::string line(buf);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
  if (im->self) im->last_reply = im->self->hooks_command(line) + "\n";
  _IO_SET_WRITE_NBYTES(ctp, nbytes);
  if (nbytes) ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
  return _RESMGR_NPARTS(0);
}

static void* service_thread(void* arg) {
  auto* im = static_cast<ControlSurface::Impl*>(arg);
  dispatch_context_t* ctp = dispatch_context_alloc(im->dpp);
  while (im->running && ctp) {
    dispatch_context_t* r = dispatch_block(ctp);
    if (!r) { if (errno == EINTR) continue; break; }
    ctp = r;
    dispatch_handler(ctp);
  }
  if (ctp) dispatch_context_free(ctp);
  return nullptr;
}

ControlSurface::ControlSurface(std::string mount_point, ControlHooks hooks)
    : mount_(std::move(mount_point)), hooks_(std::move(hooks)), impl_(new Impl) {
  impl_->self = this;
}

ControlSurface::~ControlSurface() { stop(); delete impl_; }

bool ControlSurface::start(std::string* err) {
  g_impl = impl_;
  impl_->dpp = dispatch_create();
  if (!impl_->dpp) { if (err) *err = strerror(errno); return false; }
  resmgr_attr_t rattr;
  memset(&rattr, 0, sizeof rattr);
  rattr.nparts_max = 1;
  rattr.msg_max_size = 2048;
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &impl_->connect_funcs, _RESMGR_IO_NFUNCS, &impl_->io_funcs);
  impl_->io_funcs.read = relmw_io_read;
  impl_->io_funcs.read64 = relmw_io_read;    // 8.0: _IO_READ64 is routed to read64 (sample in "Writing a Resource Manager")
  impl_->io_funcs.write = relmw_io_write;
  impl_->io_funcs.write64 = relmw_io_write;  // likewise for _IO_WRITE64
  iofunc_attr_init(&impl_->attr_status, S_IFNAM | 0444, nullptr, nullptr);
  iofunc_attr_init(&impl_->attr_events, S_IFNAM | 0444, nullptr, nullptr);
  iofunc_attr_init(&impl_->attr_ctl, S_IFNAM | 0666, nullptr, nullptr);
  impl_->id_status = resmgr_attach(impl_->dpp, &rattr, (mount_ + "/status").c_str(), _FTYPE_ANY, 0,
                                   &impl_->connect_funcs, &impl_->io_funcs, &impl_->attr_status);
  impl_->id_events = resmgr_attach(impl_->dpp, &rattr, (mount_ + "/events").c_str(), _FTYPE_ANY, 0,
                                   &impl_->connect_funcs, &impl_->io_funcs, &impl_->attr_events);
  impl_->id_ctl = resmgr_attach(impl_->dpp, &rattr, (mount_ + "/ctl").c_str(), _FTYPE_ANY, 0,
                                &impl_->connect_funcs, &impl_->io_funcs, &impl_->attr_ctl);
  if (impl_->id_status == -1 || impl_->id_events == -1 || impl_->id_ctl == -1) {
    if (err) *err = std::string("resmgr_attach: ") + strerror(errno) + " (needs PROCMGR_AID_PATHSPACE / root)";
    return false;
  }
  impl_->running = true;
  if (pthread_create(&impl_->thread, nullptr, service_thread, impl_) != EOK) {
    impl_->running = false;
    if (err) *err = "pthread_create failed";
    return false;
  }
  log(Level::Info, "resource manager up at %s/{status,events,ctl}", mount_.c_str());
  return true;
}

void ControlSurface::stop() {
  if (!impl_ || !impl_->running) return;
  impl_->running = false;
  if (impl_->id_status != -1) resmgr_detach(impl_->dpp, impl_->id_status, _RESMGR_DETACH_ALL);
  if (impl_->id_events != -1) resmgr_detach(impl_->dpp, impl_->id_events, _RESMGR_DETACH_ALL);
  if (impl_->id_ctl != -1) resmgr_detach(impl_->dpp, impl_->id_ctl, _RESMGR_DETACH_ALL);
  pthread_cancel(impl_->thread);
  pthread_join(impl_->thread, nullptr);
}

void ControlSurface::publish() {}

}  // namespace relmw::platform
#endif  // __QNX__
