// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Unit tests of compat/qnx_sim (QNX API simulation on Linux, not QNX): the message-passing, pulse, timer, resmgr and
// slog2 subset behaves the way src/platform/qnx/*.cpp expects from the QNX 8.0 documentation.
#include "../minitest.hpp"
#include "qnx_sim.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <sys/slog2.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <stdio.h>
#include <string>
#include <thread>

namespace {
std::string g_root;
struct RootInit {
  RootInit() {
    char t[] = "/tmp/qnxsim_test_XXXXXX";
    g_root = mkdtemp(t);
    setenv("QNXSIM_ROOT", g_root.c_str(), 1);
  }
} g_root_init;

long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

TEST(msgsend_receive_reply_round_trip) {
  int chid = ChannelCreate(0);
  CHECK(chid > 0);
  std::thread srv([chid] {
    for (int i = 0; i < 2; ++i) {
      char buf[64] = {0};
      struct _msg_info info;
      rcvid_t r = MsgReceive(chid, buf, sizeof buf, &info);
      if (r <= 0) return;
      if (!strcmp(buf, "fail")) { MsgError(r, EPERM); continue; }
      std::string rep = std::string("echo:") + buf + ":" + std::to_string(info.srcmsglen) + ":" + std::to_string(info.dstmsglen);
      MsgReply(r, 42, rep.c_str(), rep.size() + 1);
    }
  });
  int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
  CHECK(coid >= 0);
  char rbuf[64] = {0};
  long st = MsgSend(coid, "hello", 6, rbuf, sizeof rbuf);
  CHECK(st == 42);
  CHECK(std::string(rbuf) == "echo:hello:6:64");
  errno = 0;
  CHECK(MsgSend(coid, "fail", 5, rbuf, sizeof rbuf) == -1);
  CHECK(errno == EPERM);
  srv.join();
  ConnectDetach(coid);
  ChannelDestroy(chid);
}

TEST(two_clients_get_their_own_replies) {
  int chid = ChannelCreate(0);
  std::thread srv([&] {
    for (int i = 0; i < 200; ++i) {
      int v = 0;
      rcvid_t r = MsgReceive(chid, &v, sizeof v, nullptr);
      if (r <= 0) continue;
      int out = v * 2;
      MsgReply(r, 0, &out, sizeof out);
    }
  });
  auto client = [&](int base, bool* ok) {
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    *ok = coid >= 0;
    for (int i = 0; i < 100 && *ok; ++i) {
      int v = base + i, out = -1;
      if (MsgSend(coid, &v, sizeof v, &out, sizeof out) != 0 || out != 2 * v) *ok = false;
    }
    ConnectDetach(coid);
  };
  bool ok1 = false, ok2 = false;
  std::thread a(client, 1000, &ok1), b(client, 5000, &ok2);
  a.join(); b.join(); srv.join();
  CHECK(ok1);
  CHECK(ok2);
  ChannelDestroy(chid);
}

TEST(pulse_code_and_value) {
  int chid = ChannelCreate(_NTO_CHF_PRIVATE);
  int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
  CHECK(MsgSendPulse(coid, 10, _PULSE_CODE_MINAVAIL + 3, 1234) == 0);
  struct _pulse p;
  memset(&p, 0, sizeof p);
  CHECK(MsgReceivePulse(chid, &p, sizeof p, nullptr) == 0);
  CHECK(p.code == _PULSE_CODE_MINAVAIL + 3);
  CHECK((long)(intptr_t)p.value.sival_ptr == 1234);
  ConnectDetach(coid);
  ChannelDestroy(chid);
}

TEST(timer_pulses_arrive_on_the_period_grid_and_stop_after_delete) {
  int chid = ChannelCreate(_NTO_CHF_PRIVATE);
  int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
  struct sigevent ev;
  memset(&ev, 0, sizeof ev);
  SIGEV_PULSE_INIT(&ev, coid, 10, _PULSE_CODE_MINAVAIL + 1, 7);
  CHECK(MsgRegisterEvent(&ev, SYSMGR_COID) == 0);
  timer_t t;
  CHECK(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0);
  struct itimerspec its;
  memset(&its, 0, sizeof its);
  nsec2timespec(&its.it_value, 2000000);
  nsec2timespec(&its.it_interval, 2000000);
  long t0 = now_ms();
  CHECK(timer_settime(t, 0, &its, nullptr) == 0);
  int got = 0;
  for (int i = 0; i < 10; ++i) {
    struct _pulse p;
    if (MsgReceivePulse(chid, &p, sizeof p, nullptr) == 0 && p.code == _PULSE_CODE_MINAVAIL + 1 &&
        (long)(intptr_t)p.value.sival_ptr == 7) ++got;
  }
  long dt = now_ms() - t0;
  CHECK(got == 10);
  CHECK(dt >= 15 && dt <= 60);
  CHECK(timer_delete(t) == 0);
  // After timer_delete no more pulses: a sentinel pulse sent now must be the next thing received.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  struct _pulse p;
  while (true) {   // drain pulses that were already queued before the delete
    MsgSendPulse(coid, 10, _PULSE_CODE_MINAVAIL + 9, 0);
    int seen_tick = 0;
    for (;;) {
      MsgReceivePulse(chid, &p, sizeof p, nullptr);
      if (p.code == _PULSE_CODE_MINAVAIL + 9) break;
      ++seen_tick;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    MsgSendPulse(coid, 10, _PULSE_CODE_MINAVAIL + 9, 0);
    MsgReceivePulse(chid, &p, sizeof p, nullptr);
    CHECK(p.code == _PULSE_CODE_MINAVAIL + 9);   // nothing arrived in 10 ms after the drain
    break;
  }
  ConnectDetach(coid);
  ChannelDestroy(chid);
}

// ---- resource manager ----------------------------------------------------------------------------------------------
namespace {
iofunc_attr_t g_attr;
std::string g_text = "line one\nline two\n";
std::string g_written;
std::atomic<bool> g_srv_quit{false};   // set by the server thread itself (no race with the test thread)
int rd(resmgr_context_t* ctp, io_read_t* msg, RESMGR_OCB_T* ocb) {
  if (_IO_READ_GET_NBYTES(msg) == 7777) g_srv_quit = true;   // sentinel read: last message the server handles
  int st = iofunc_read_verify(ctp, msg, ocb, nullptr);
  if (st != EOK) return st;
  size_t off = (size_t)ocb->offset;
  size_t n = off < g_text.size() ? g_text.size() - off : 0;
  if (n > _IO_READ_GET_NBYTES(msg)) n = _IO_READ_GET_NBYTES(msg);
  SETIOV(ctp->iov, g_text.data() + off, n);
  _IO_SET_READ_NBYTES(ctp, n);
  ocb->offset += n;
  return _RESMGR_NPARTS(n ? 1 : 0);
}
int wr(resmgr_context_t* ctp, io_write_t* msg, RESMGR_OCB_T* ocb) {
  int st = iofunc_write_verify(ctp, msg, ocb, nullptr);
  if (st != EOK) return st;
  size_t n = _IO_WRITE_GET_NBYTES(msg);
  char buf[256];
  ssize_t got = resmgr_msgget(ctp, buf, n < sizeof buf ? n : sizeof buf, sizeof(msg->i));
  g_written.assign(buf, got > 0 ? (size_t)got : 0);
  _IO_SET_WRITE_NBYTES(ctp, n);
  return _RESMGR_NPARTS(0);
}
long read_all(int fd, std::string& out, size_t chunk) {
  out.clear();
  for (;;) {
    io_read_t m;
    memset(&m, 0, sizeof m);
    m.i.type = _IO_READ;
    m.i.nbytes = (uint32_t)chunk;
    char buf[64];
    long n = MsgSend(fd, &m, sizeof m.i, buf, chunk);
    if (n <= 0) return n;
    out.append(buf, (size_t)n);
  }
}
}  // namespace

TEST(resmgr_read_write_msg_and_open_modes) {
  dispatch_t* dpp = dispatch_create();
  CHECK(dpp != nullptr);
  resmgr_connect_funcs_t cf;
  resmgr_io_funcs_t io;
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &io);
  io.read = rd;
  io.write = wr;
  iofunc_attr_init(&g_attr, S_IFNAM | 0666, nullptr, nullptr);
  resmgr_attr_t ra;
  memset(&ra, 0, sizeof ra);
  ra.nparts_max = 1;
  ra.msg_max_size = 2048;
  int id = resmgr_attach(dpp, &ra, "/dev/simtest", _FTYPE_ANY, 0, &cf, &io, &g_attr);
  CHECK(id >= 0);
  struct stat st;
  CHECK(stat((g_root + "/dev/simtest").c_str(), &st) == 0);
  std::thread srv([&] {
    dispatch_context_t* ctp = dispatch_context_alloc(dpp);
    while (!g_srv_quit) {
      dispatch_context_t* r = dispatch_block(ctp);
      if (!r) continue;
      ctp = r;
      dispatch_handler(ctp);
    }
    dispatch_context_free(ctp);
  });
  int fd = open("/dev/simtest", O_RDONLY);
  CHECK(fd >= 0);
  std::string got;
  CHECK(read_all(fd, got, 5) == 0);            // 5-byte chunks: offsets must advance per OCB
  CHECK(got == g_text);
  // write on an O_RDONLY OCB -> EBADF from iofunc_write_verify
  io_write_t h;
  memset(&h, 0, sizeof h);
  h.i.type = _IO_WRITE;
  h.i.nbytes = 3;
  char m1[sizeof h + 3];
  memcpy(m1, &h, sizeof h);
  memcpy(m1 + sizeof h, "abc", 3);
  errno = 0;
  CHECK(MsgSend(fd, m1, sizeof m1, nullptr, 0) == -1 && errno == EBADF);
  // _IO_MSG without a handler -> ENOSYS
  io_msg_t om;
  memset(&om, 0, sizeof om);
  om.i.type = _IO_MSG;
  errno = 0;
  CHECK(MsgSend(fd, &om, sizeof om, nullptr, 0) == -1 && errno == ENOSYS);
  close(fd);
  int wfd = open("/dev/simtest", O_WRONLY);
  CHECK(wfd >= 0);
  long wn = MsgSend(wfd, m1, sizeof m1, nullptr, 0);
  CHECK(wn == 3);
  CHECK(g_written == "abc");
  io_read_t rm;
  memset(&rm, 0, sizeof rm);
  rm.i.type = _IO_READ;
  rm.i.nbytes = 8;
  char b[8];
  errno = 0;
  CHECK(MsgSend(wfd, &rm, sizeof rm.i, b, sizeof b) == -1 && errno == EBADF);
  close(wfd);
  int nfd = open("/dev/nonexistent_simtest", O_RDONLY);
  CHECK(nfd == -1 && errno == ENOENT);   // falls through to the real open()
  int kick = open("/dev/simtest", O_RDONLY);
  CHECK(kick >= 0);
  if (kick >= 0) {   // sentinel read: the server thread exits after handling it
    io_read_t km;
    memset(&km, 0, sizeof km);
    km.i.type = _IO_READ;
    km.i.nbytes = 7777;
    char kb[8];
    MsgSend(kick, &km, sizeof km.i, kb, sizeof kb);
    close(kick);
  }
  srv.join();
  CHECK(resmgr_detach(dpp, id, _RESMGR_DETACH_ALL) == 0);
  CHECK(stat((g_root + "/dev/simtest").c_str(), &st) != 0);
  dispatch_destroy(dpp);
}

TEST(slog2_writes_the_buffer_file) {
  slog2_buffer_set_config_t cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.buffer_set_name = "simtest";
  cfg.num_buffers = 1;
  cfg.verbosity_level = SLOG2_INFO;
  cfg.buffer_config[0].buffer_name = "simtest";
  cfg.buffer_config[0].num_pages = 4;
  slog2_buffer_t h;
  CHECK(slog2_register(&cfg, &h, 0) == 0);
  CHECK(slog2f(h, 0, SLOG2_INFO, "value=%d", 17) == 0);
  CHECK(slog2f(h, 0, SLOG2_DEBUG2, "%s", "filtered") == 0);   // above verbosity: dropped
  // (no <fstream> here: the simulation's <fcntl.h> renames open(), see compat/qnx_sim/README.md)
  std::string text;
  if (FILE* f = fopen((g_root + "/slog2/simtest.log").c_str(), "r")) {
    char line[512];
    while (fgets(line, sizeof line, f)) text += line;
    fclose(f);
  }
  CHECK(text.find("value=17") != std::string::npos);
  CHECK(text.find("INFO") != std::string::npos);
  CHECK(text.find("filtered") == std::string::npos);
}

TEST(clusters_and_runmask) {
  uint64_t m = 0;
  CHECK(pthread_cluster_get_runmask("_cpu-0", &m) == EOK && m == 1);
  CHECK(pthread_cluster_get_runmask("bogus", &m) == EINVAL);
  CHECK(pthread_cluster_set(0, "_cpu-0") == EOK);
  unsigned rm = 0;
  CHECK(ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET, &rm) == 0 && rm == 1u);
  CHECK(pthread_cluster_set(0, "_all") == EOK);
}

TEST_MAIN()
