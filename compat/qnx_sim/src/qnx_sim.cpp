// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX API simulation on Linux, not QNX.
//
// A user-space stand-in for the small subset of the QNX 8.0 API that src/platform/qnx/*.cpp uses, so that the SAME
// QNX sources compile, link and run end to end on Linux (logic tests, codelab). Nothing here measures or proves QNX
// behaviour: scheduling, priority inheritance, blocking states, security and timing are Linux's. The differences are
// listed in compat/qnx_sim/README.md.
//
// Transport: every channel is an AF_UNIX SOCK_SEQPACKET listening socket (abstract name); a connection id is a real
// fd. Each simulated message is ONE seqpacket record: a Frame header followed by the payload. MsgSend() sends a MSG
// record and blocks reading the REPLY record on the same connection; the receiver's rcvid is the accepted fd.
// resmgr_attach() additionally binds a filesystem socket at <root><path>; qnxsim_open() connects to it and sends a
// CONNECT record, which the dispatch layer turns into an OCB.
#include "qnx_sim.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/eventfd.h>
#include <sys/iofunc.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <sys/siginfo.h>
#include <sys/slog2.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <ha/ham.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#undef open   // this file implements qnxsim_open() and needs the real open()

namespace {

constexpr uint32_t kMagic = 0x514e5853;  // "QNXS"
enum Kind : uint16_t { kMsg = 1, kPulse = 2, kReply = 3, kConnect = 4, kDisconnect = 5 };  // kDisconnect: local only

struct Frame {
  uint32_t magic = kMagic;
  uint16_t kind = 0;
  int16_t code = 0;
  int32_t err = 0;
  int32_t ioflag = 0;
  int64_t val = 0;      // pulse value / reply status
  uint32_t len = 0;     // payload bytes that follow
  uint32_t dstlen = 0;  // MSG: size of the sender's reply buffer
};

bool send_frame(int fd, const Frame& f, const void* data, size_t len, int flags = 0) {
  struct iovec iov[2];
  iov[0].iov_base = const_cast<Frame*>(&f);
  iov[0].iov_len = sizeof f;
  iov[1].iov_base = const_cast<void*>(data);
  iov[1].iov_len = len;
  struct msghdr mh;
  memset(&mh, 0, sizeof mh);
  mh.msg_iov = iov;
  mh.msg_iovlen = len ? 2 : 1;
  for (;;) {
    ssize_t n = sendmsg(fd, &mh, MSG_NOSIGNAL | flags);
    if (n >= 0) return true;
    if (errno != EINTR) return false;
  }
}

// Receives one record. Returns 1 on success, 0 on orderly EOF, -1 on error (errno set; EINTR is passed through).
int recv_frame(int fd, Frame& f, std::vector<char>& payload) {
  ssize_t n = recv(fd, nullptr, 0, MSG_PEEK | MSG_TRUNC);
  if (n == 0) return 0;
  if (n < 0) return -1;
  if ((size_t)n < sizeof f) { recv(fd, nullptr, 0, 0); errno = EBADMSG; return -1; }
  char small[512];   // records up to 512 bytes (pulses, short messages) stay off the heap
  std::vector<char> big;
  char* data = small;
  if ((size_t)n > sizeof small) { big.resize((size_t)n); data = big.data(); }
  ssize_t m = recv(fd, data, (size_t)n, 0);
  if (m < 0) return -1;
  if (m == 0) return 0;
  memcpy(&f, data, sizeof f);
  if (f.magic != kMagic) { errno = EBADMSG; return -1; }
  payload.assign(data + sizeof f, data + m);
  return 1;
}

std::string abstract_name(int chid) {
  static std::atomic<int> seq{0};
  char b[96];
  snprintf(b, sizeof b, "qnxsim.%ld.%d.%d", (long)getpid(), chid, seq.fetch_add(1));
  return b;
}

int make_listener(const std::string& name, bool abstract) {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un a;
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  socklen_t alen;
  if (abstract) {
    if (name.size() + 1 >= sizeof a.sun_path) { close(fd); errno = ENAMETOOLONG; return -1; }
    memcpy(a.sun_path + 1, name.data(), name.size());
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name.size());
  } else {
    if (name.size() >= sizeof a.sun_path) { close(fd); errno = ENAMETOOLONG; return -1; }
    memcpy(a.sun_path, name.data(), name.size());
    alen = (socklen_t)sizeof a;
  }
  if (bind(fd, (struct sockaddr*)&a, alen) != 0 || listen(fd, 64) != 0) { int e = errno; close(fd); errno = e; return -1; }
  return fd;
}

int connect_to(const std::string& name, bool abstract) {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un a;
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  socklen_t alen;
  if (abstract) {
    memcpy(a.sun_path + 1, name.data(), std::min(name.size(), sizeof a.sun_path - 2));
    alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name.size());
  } else {
    if (name.size() >= sizeof a.sun_path) { close(fd); errno = ENAMETOOLONG; return -1; }
    memcpy(a.sun_path, name.data(), name.size());
    alen = (socklen_t)sizeof a;
  }
  if (connect(fd, (struct sockaddr*)&a, alen) != 0) { int e = errno; close(fd); errno = e; return -1; }
  return fd;
}

// ---- channels ---------------------------------------------------------------------------------------------------
struct Received {
  Kind kind = kMsg;
  int fd = -1;          // connection the record came from (rcvid for messages)
  int attach_id = -1;   // resmgr attach id of the listener the connection came through (-1: channel itself)
  Frame f;
  std::vector<char> payload;
};

struct Channel {
  int chid = 0;
  unsigned flags = 0;
  std::string name;                       // abstract socket name
  std::mutex mu;                          // guards the vectors/maps below (never held across poll)
  std::vector<std::pair<int, int>> listeners;  // (fd, attach id)
  std::map<int, int> conns;               // fd -> attach id
  std::vector<Received> deferred;         // messages that arrived during MsgReceivePulse()
  int wakefd = -1;                        // eventfd: ConnectAttach() added a connection, re-build the poll set
};

std::mutex g_mu;
std::map<int, std::shared_ptr<Channel>> g_channels;
int g_next_chid = 1;

std::shared_ptr<Channel> find_channel(int chid) {
  std::lock_guard<std::mutex> g(g_mu);
  auto it = g_channels.find(chid);
  return it == g_channels.end() ? nullptr : it->second;
}

// Blocks until a record arrives on the channel. pulses_only: messages are parked in ch.deferred (QNX leaves the
// senders SEND-blocked until a MsgReceive()). Returns 0 or -1 with errno (EINTR on a signal).
int channel_receive(Channel& ch, Received& out, bool pulses_only) {
  if (!pulses_only) {
    std::lock_guard<std::mutex> g(ch.mu);
    if (!ch.deferred.empty()) { out = std::move(ch.deferred.front()); ch.deferred.erase(ch.deferred.begin()); return 0; }
  }
  for (;;) {
    // Small fixed arrays on the stack: the steady-state receive path does not touch the heap, which keeps the
    // QNX code's rt_noalloc test meaningful under the simulation. More than kMaxFds fds: the rest wait a round.
    constexpr size_t kMaxFds = 64;
    struct pollfd pfa[kMaxFds];
    int attach[kMaxFds];
    size_t npf = 0, nlisten;
    {
      std::lock_guard<std::mutex> g(ch.mu);
      if (ch.wakefd >= 0) { pfa[npf] = {ch.wakefd, POLLIN, 0}; attach[npf++] = -2; }
      for (auto& l : ch.listeners) if (npf < kMaxFds) { pfa[npf] = {l.first, POLLIN, 0}; attach[npf++] = l.second; }
      nlisten = npf;
      for (auto& c : ch.conns) if (npf < kMaxFds) { pfa[npf] = {c.first, POLLIN, 0}; attach[npf++] = c.second; }
    }
    struct pollfd* pf = pfa;
    int r = poll(pf, npf, -1);
    if (r < 0) return -1;   // EINTR included: the caller decides (dispatch_block() returns NULL with errno EINTR)
    for (size_t i = 0; i < npf; ++i) {
      if (!pf[i].revents) continue;
      if (attach[i] == -2) { uint64_t v; ssize_t rr = read(pf[i].fd, &v, sizeof v); (void)rr; continue; }
      if (i < nlisten) {
        int c = accept4(pf[i].fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (c >= 0) { std::lock_guard<std::mutex> g(ch.mu); ch.conns[c] = attach[i]; }
        continue;
      }
      Received rv;
      int k = recv_frame(pf[i].fd, rv.f, rv.payload);
      if (k <= 0) {
        if (k < 0 && errno == EINTR) return -1;
        // Client gone. The fd is handed to the caller still open, so its number cannot be reused by a new
        // connection before the caller has dropped the per-connection state (OCB); the caller closes it.
        { std::lock_guard<std::mutex> g(ch.mu); ch.conns.erase(pf[i].fd); }
        out = Received();
        out.kind = kDisconnect;
        out.fd = pf[i].fd;
        out.attach_id = attach[i];
        return 0;
      }
      rv.kind = (Kind)rv.f.kind;
      rv.fd = pf[i].fd;
      rv.attach_id = attach[i];
      if (pulses_only && rv.kind != kPulse) {   // (disconnects are returned above)
        std::lock_guard<std::mutex> g(ch.mu);
        ch.deferred.push_back(std::move(rv));
        continue;
      }
      out = std::move(rv);
      return 0;
    }
  }
}

void fill_pulse(const Received& rv, void* pulse, size_t bytes) {
  struct _pulse p;
  memset(&p, 0, sizeof p);
  p.type = _PULSE_TYPE;
  p.code = (int8_t)rv.f.code;
  p.value.sival_ptr = (void*)(intptr_t)rv.f.val;
  memcpy(pulse, &p, std::min(bytes, sizeof p));
}

void fill_info(const Received& rv, struct _msg_info* info, size_t copied) {
  if (!info) return;
  memset(info, 0, sizeof *info);
  info->pid = getpid();   // the simulation does not know the peer pid (SO_PEERCRED would; not needed here)
  info->msglen = copied;
  info->srcmsglen = rv.payload.size();
  info->dstmsglen = rv.f.dstlen;
}

std::mutex g_coid_mu;
std::map<int, std::shared_ptr<std::mutex>> g_coid_locks;   // one MsgSend() at a time per connection
std::shared_ptr<std::mutex> coid_lock(int coid) {
  std::lock_guard<std::mutex> g(g_coid_mu);
  auto& p = g_coid_locks[coid];
  if (!p) p = std::make_shared<std::mutex>();
  return p;
}

}  // namespace

// ---- kernel calls -------------------------------------------------------------------------------------------------
extern "C" {

const char* qnxsim_root(void) {
  static std::string root = [] {
    const char* e = getenv("QNXSIM_ROOT");
    if (e && *e) return std::string(e);
    return std::string("/tmp/qnxsim-") + std::to_string((long)getuid());
  }();
  return root.c_str();
}

int ChannelCreate(unsigned flags) {
  auto ch = std::make_shared<Channel>();
  std::lock_guard<std::mutex> g(g_mu);
  ch->chid = g_next_chid++;
  ch->flags = flags;
  ch->name = abstract_name(ch->chid);
  int fd = make_listener(ch->name, true);
  if (fd < 0) return -1;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);   // ConnectAttach() and the receiver may both accept
  ch->listeners.push_back({fd, -1});
  ch->wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  g_channels[ch->chid] = ch;
  return ch->chid;
}

int ChannelDestroy(int chid) {
  std::shared_ptr<Channel> ch;
  {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_channels.find(chid);
    if (it == g_channels.end()) { errno = EINVAL; return -1; }
    ch = it->second;
    g_channels.erase(it);
  }
  std::lock_guard<std::mutex> g(ch->mu);
  for (auto& l : ch->listeners) close(l.first);
  for (auto& c : ch->conns) close(c.first);
  ch->listeners.clear();
  ch->conns.clear();
  if (ch->wakefd >= 0) { close(ch->wakefd); ch->wakefd = -1; }
  return 0;
}

int ConnectAttach(uint32_t, pid_t pid, int chid, unsigned, int) {
  if (pid != 0 && pid != getpid()) { errno = ESRCH; return -1; }   // only connections inside this process
  auto ch = find_channel(chid);
  if (!ch) { errno = ESRCH; return -1; }
  int coid = connect_to(ch->name, true);
  if (coid < 0) return -1;
  // Accept the server end right away (all connections to a channel are equivalent), so the connection already
  // exists when ConnectAttach() returns, as on QNX, and the receive path never allocates for it later.
  std::lock_guard<std::mutex> g(ch->mu);
  if (!ch->listeners.empty()) {
    int c = accept4(ch->listeners.front().first, nullptr, nullptr, SOCK_CLOEXEC);
    if (c >= 0) {
      ch->conns[c] = -1;
      uint64_t one = 1;
      ssize_t wr = write(ch->wakefd, &one, sizeof one);   // a receiver already in poll() must watch the new fd
      (void)wr;
    }
  }
  return coid;
}

int ConnectDetach(int coid) { return close(coid); }

long MsgSend(int coid, const void* smsg, size_t sbytes, void* rmsg, size_t rbytes) {
  auto lk = coid_lock(coid);
  std::lock_guard<std::mutex> g(*lk);
  Frame f;
  f.kind = kMsg;
  f.len = (uint32_t)sbytes;
  f.dstlen = (uint32_t)rbytes;
  if (!send_frame(coid, f, smsg, sbytes)) { if (errno == EPIPE || errno == ECONNRESET) errno = ESRCH; return -1; }
  Frame r;
  std::vector<char> payload;
  for (;;) {
    int k = recv_frame(coid, r, payload);
    if (k > 0) break;
    if (k < 0 && errno == EINTR) continue;   // QNX: a REPLY-blocked thread is not interrupted without _NTO_CHF_UNBLOCK
    errno = ESRCH;                            // server went away
    return -1;
  }
  if (r.kind != kReply) { errno = EBADMSG; return -1; }
  if (r.err) { errno = r.err; return -1; }
  if (rmsg && rbytes) memcpy(rmsg, payload.data(), std::min(rbytes, payload.size()));
  return (long)r.val;
}

int MsgSendPulse(int coid, int, int code, long value) {
  Frame f;
  f.kind = kPulse;
  f.code = (int16_t)code;
  f.val = value;
  if (!send_frame(coid, f, nullptr, 0, MSG_DONTWAIT)) return -1;
  return 0;
}

rcvid_t MsgReceive(int chid, void* msg, size_t bytes, struct _msg_info* info) {
  auto ch = find_channel(chid);
  if (!ch) { errno = EINVAL; return -1; }
  for (;;) {
    Received rv;
    if (channel_receive(*ch, rv, false) != 0) return -1;
    if (rv.kind == kDisconnect) { close(rv.fd); continue; }   // QNX would send a _PULSE_CODE_DISCONNECT if asked
    if (rv.kind == kPulse) { fill_pulse(rv, msg, bytes); if (info) memset(info, 0, sizeof *info); return 0; }
    if (rv.kind == kConnect) {   // plain channel: nothing to open, just let the client proceed
      Frame r; r.kind = kReply;
      send_frame(rv.fd, r, nullptr, 0);
      continue;
    }
    size_t n = std::min(bytes, rv.payload.size());
    if (n) memcpy(msg, rv.payload.data(), n);
    fill_info(rv, info, n);
    return rv.fd;
  }
}

int MsgReceivePulse(int chid, void* pulse, size_t bytes, struct _msg_info* info) {
  auto ch = find_channel(chid);
  if (!ch) { errno = EINVAL; return -1; }
  Received rv;
  for (;;) {
    if (channel_receive(*ch, rv, true) != 0) return -1;
    if (rv.kind != kDisconnect) break;
    close(rv.fd);
  }
  fill_pulse(rv, pulse, bytes);
  if (info) memset(info, 0, sizeof *info);
  return 0;
}

int MsgReply(rcvid_t rcvid, long status, const void* msg, size_t bytes) {
  Frame r;
  r.kind = kReply;
  r.val = status;
  r.len = msg ? (uint32_t)bytes : 0;
  if (!send_frame((int)rcvid, r, msg, msg ? bytes : 0)) { errno = ESRCH; return -1; }
  return 0;
}

int MsgError(rcvid_t rcvid, int error) {
  Frame r;
  r.kind = kReply;
  r.err = error;
  if (!send_frame((int)rcvid, r, nullptr, 0)) { errno = ESRCH; return -1; }
  return 0;
}

int MsgRegisterEvent(struct sigevent*, int) { return 0; }   // secure events are not simulated: event used as-is
int MsgUnregisterEvent(const struct sigevent*) { return 0; }

// ---- timers (SIGEV_PULSE only) --------------------------------------------------------------------------------------
}  // extern "C"

namespace {
struct SimTimer {
  int coid = -1, prio = 0, code = 0;
  long value = 0;
  std::thread th;
  std::mutex m;
  std::condition_variable cv;
  bool stop = false;
  void halt() {
    { std::lock_guard<std::mutex> g(m); stop = true; }
    cv.notify_all();
    if (th.joinable()) th.join();
    stop = false;
  }
};
}  // namespace

extern "C" {

int qnxsim_timer_create(clockid_t clock_id, struct sigevent* evp, timer_t* timerid) {
  if (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_REALTIME) { errno = EINVAL; return -1; }
  if (!evp || evp->sigev_notify != SIGEV_PULSE || !timerid) { errno = ENOTSUP; return -1; }   // pulses only
  auto* t = new SimTimer;
  t->coid = evp->sigev_signo;
  t->prio = evp->_sigev_un._pad[0];
  t->code = evp->_sigev_un._pad[1];
  t->value = (long)(intptr_t)evp->sigev_value.sival_ptr;
  *timerid = (timer_t)t;
  return 0;
}

int qnxsim_timer_settime(timer_t timerid, int flags, const struct itimerspec* value, struct itimerspec* ovalue) {
  auto* t = (SimTimer*)timerid;
  if (!t || !value) { errno = EINVAL; return -1; }
  if (ovalue) memset(ovalue, 0, sizeof *ovalue);
  t->halt();
  using clk = std::chrono::steady_clock;   // CLOCK_MONOTONIC on Linux
  auto ns = [](const struct timespec& ts) { return std::chrono::nanoseconds((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec); };
  if (value->it_value.tv_sec == 0 && value->it_value.tv_nsec == 0) return 0;   // disarm
  auto first = ns(value->it_value);
  auto period = ns(value->it_interval);
  clk::time_point next;
  if (flags & TIMER_ABSTIME) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    next = clk::now() + (first - ns(now));
  } else {
    next = clk::now() + first;
  }
  t->th = std::thread([t, next, period]() mutable {
    std::unique_lock<std::mutex> lk(t->m);
    for (;;) {
      if (t->cv.wait_until(lk, next, [t] { return t->stop; })) return;
      MsgSendPulse(t->coid, t->prio, t->code, t->value);
      if (period.count() == 0) return;
      next += period;
      auto now = clk::now();
      while (next + period <= now) next += period;   // overruns are absorbed (QNX counts them; not simulated)
    }
  });
  return 0;
}

int qnxsim_timer_delete(timer_t timerid) {
  auto* t = (SimTimer*)timerid;
  if (!t) { errno = EINVAL; return -1; }
  t->halt();
  delete t;
  return 0;
}

void nsec2timespec(struct timespec* ts, uint64_t nsec) {
  ts->tv_sec = (time_t)(nsec / 1000000000ULL);
  ts->tv_nsec = (long)(nsec % 1000000000ULL);
}

// ---- runmask / clusters --------------------------------------------------------------------------------------------
static const cpu_set_t& initial_mask() {
  static cpu_set_t m;
  static std::once_flag once;
  std::call_once(once, [] { CPU_ZERO(&m); if (sched_getaffinity(0, sizeof m, &m) != 0) CPU_ZERO(&m); });
  return m;
}

int pthread_cluster_get_runmask(char const* name, uint64_t* runmask) {
  const cpu_set_t& all = initial_mask();
  uint64_t mask = 0;
  std::string n = name ? name : "";
  if (n == "_all") {
    for (int i = 0; i < 64; ++i) if (CPU_ISSET(i, &all)) mask |= 1ULL << i;
  } else if (n.rfind("_cpu-", 0) == 0 && n.size() > 5 && n.find_first_not_of("0123456789", 5) == std::string::npos &&
             n.size() < 8) {
    int c = atoi(n.c_str() + 5);
    if (c >= 64 || !CPU_ISSET(c, &all)) return EINVAL;
    mask = 1ULL << c;
  } else {
    return EINVAL;   // custom clusters come from the QNX startup program (-c); none exist here
  }
  if (runmask) *runmask = mask;
  return EOK;
}

int pthread_cluster_set(pthread_t tid, char const* name) {
  uint64_t mask = 0;
  int rc = pthread_cluster_get_runmask(name, &mask);
  if (rc != EOK) return rc;
  if (tid != 0 && !pthread_equal(tid, pthread_self())) return EINVAL;   // QNX: "Set this argument to zero"
  cpu_set_t s;
  CPU_ZERO(&s);
  for (int i = 0; i < 64; ++i) if (mask & (1ULL << i)) CPU_SET(i, &s);
  return pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

int pthread_cluster_set_and_inherit(pthread_t tid, char const* name, char const*) { return pthread_cluster_set(tid, name); }

int ThreadCtl(int cmd, void* data) {
  if (cmd != _NTO_TCTL_RUNMASK_GET_AND_SET) { errno = ENOTSUP; return -1; }   // the only ThreadCtl command simulated
  unsigned* p = (unsigned*)data;
  cpu_set_t cur;
  CPU_ZERO(&cur);
  if (pthread_getaffinity_np(pthread_self(), sizeof cur, &cur) != 0) { errno = EINVAL; return -1; }
  unsigned old = 0;
  for (int i = 0; i < 32; ++i) if (CPU_ISSET(i, &cur)) old |= 1u << i;
  unsigned want = p ? *p : 0;
  if (want) {
    cpu_set_t s;
    CPU_ZERO(&s);
    for (int i = 0; i < 32; ++i) if (want & (1u << i)) CPU_SET(i, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof s, &s) != 0) { errno = EINVAL; return -1; }
  }
  if (p) *p = old;
  return 0;
}

// ---- slog2 subset --------------------------------------------------------------------------------------------------
}  // extern "C"
namespace {
struct SlogBuf { FILE* f = nullptr; std::string set; uint8_t verbosity = SLOG2_INFO; std::mutex m; };
const char* sev_name(uint8_t s) {
  static const char* n[] = {"SHUTDOWN", "CRITICAL", "ERROR", "WARNING", "NOTICE", "INFO", "DEBUG1", "DEBUG2"};
  return s < 8 ? n[s] : "?";
}
void mkdirs(const std::string& path) {
  for (size_t i = 1; i < path.size(); ++i)
    if (path[i] == '/') { std::string p = path.substr(0, i); mkdir(p.c_str(), 0755); }
  mkdir(path.c_str(), 0755);
}
}  // namespace
extern "C" {

int slog2_register(slog2_buffer_set_config_t* config, slog2_buffer_t* handles, uint32_t) {
  if (!config || !handles || config->num_buffers < 1 || config->num_buffers > SLOG2_MAX_BUFFERS) { errno = EINVAL; return -1; }
  std::string dir = std::string(qnxsim_root()) + "/slog2";
  mkdirs(dir);
  std::string set = config->buffer_set_name ? config->buffer_set_name : "unnamed";
  std::string file = dir + "/" + set + ".log";
  for (int i = 0; i < config->num_buffers; ++i) {
    auto* b = new SlogBuf;
    b->f = fopen(file.c_str(), "a");
    if (!b->f) { delete b; return -1; }
    b->set = set;
    b->verbosity = config->verbosity_level;
    handles[i] = b;
  }
  return 0;
}

int vslog2f(slog2_buffer_t buffer, uint16_t code, uint8_t severity, const char* format, va_list arg) {
  auto* b = (SlogBuf*)buffer;
  if (!b) { errno = EINVAL; return -1; }
  if (severity > b->verbosity) return 0;
  char msg[1024];
  vsnprintf(msg, sizeof msg, format, arg);
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm;
  localtime_r(&ts.tv_sec, &tm);
  char when[32];
  strftime(when, sizeof when, "%b %d %H:%M:%S", &tm);
  std::lock_guard<std::mutex> g(b->m);
  fprintf(b->f, "%s.%03ld  %-16s %5ld  %-8s %u  %s\n", when, ts.tv_nsec / 1000000, b->set.c_str(), (long)getpid(),
          sev_name(severity), (unsigned)code, msg);
  fflush(b->f);
  return 0;
}

int slog2f(slog2_buffer_t buffer, uint16_t code, uint8_t severity, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  int r = vslog2f(buffer, code, severity, format, ap);
  va_end(ap);
  return r;
}

int slog2c(slog2_buffer_t buffer, uint16_t code, uint8_t severity, const char* data) {
  return slog2f(buffer, code, severity, "%s", data);
}

// ---- HAM: not simulated --------------------------------------------------------------------------------------------
static void ham_note() {
  static std::once_flag once;
  std::call_once(once, [] { fprintf(stderr, "qnx_sim: HAM not simulated (calls succeed and do nothing)\n"); });
}
static char g_ham_dummy[3];
int ham_connect(unsigned) { ham_note(); return 0; }
int ham_disconnect(unsigned) { return 0; }
ham_entity_t* ham_attach(const char*, int, pid_t, const char*, unsigned) { ham_note(); return (ham_entity_t*)&g_ham_dummy[0]; }
ham_entity_t* ham_attach_self(const char*, uint64_t, int, int, unsigned) { ham_note(); return (ham_entity_t*)&g_ham_dummy[0]; }
int ham_detach_self(ham_entity_t*, unsigned) { return 0; }
int ham_heartbeat(void) { return 0; }
ham_condition_t* ham_condition(ham_entity_t*, int, const char*, unsigned) { return (ham_condition_t*)&g_ham_dummy[1]; }
ham_action_t* ham_action_restart(ham_condition_t*, const char*, const char*, unsigned) { return (ham_action_t*)&g_ham_dummy[2]; }
ham_action_t* ham_action_execute(ham_condition_t*, const char*, const char*, unsigned) { return (ham_action_t*)&g_ham_dummy[2]; }
ham_action_t* ham_action_log(ham_condition_t*, const char*, const char*, unsigned, int, unsigned) { return (ham_action_t*)&g_ham_dummy[2]; }

}  // extern "C"

// ---- resource managers (minimal dispatch/iofunc subset) -------------------------------------------------------------
namespace {
struct Attach {
  int id = -1;
  std::string path, sock;
  int fd = -1;
  const resmgr_connect_funcs_t* cfuncs = nullptr;
  const resmgr_io_funcs_t* iofuncs = nullptr;
  iofunc_attr_t* handle = nullptr;
};
}  // namespace

struct _dispatch {
  int chid = -1;
  bool own_chid = false;
  std::shared_ptr<Channel> ch;
  std::mutex mu;
  std::vector<Attach> attaches;
  std::map<int, iofunc_ocb_t*> ocbs;   // connection fd -> OCB created by its CONNECT
  size_t msg_max = 2048;
  int next_id = 0;
};

namespace {
struct CtxExt {
  dispatch_context_t c;       // must stay first: ctp points here
  std::vector<char> buf;      // ctp->msg (msg_max_size bytes)
  std::vector<char> payload;  // the whole received message, for resmgr_msgget()
};
CtxExt* ext_of(resmgr_context_t* ctp) { return reinterpret_cast<CtxExt*>(ctp); }

Attach* find_attach(dispatch_t* dpp, int id) {
  for (auto& a : dpp->attaches) if (a.id == id) return &a;
  return nullptr;
}

}  // namespace

extern "C" {

dispatch_t* dispatch_create_channel(int chid, unsigned) {
  auto ch = find_channel(chid);
  if (!ch) { errno = EINVAL; return nullptr; }
  auto* d = new dispatch_t;
  d->chid = chid;
  d->ch = ch;
  return d;
}

dispatch_t* dispatch_create(void) {
  int chid = ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT);
  if (chid == -1) return nullptr;
  dispatch_t* d = dispatch_create_channel(chid, 0);
  if (d) d->own_chid = true;
  return d;
}

int dispatch_destroy(dispatch_t* dpp) {
  if (!dpp) { errno = EINVAL; return -1; }
  for (auto& a : dpp->attaches) resmgr_detach(dpp, a.id, _RESMGR_DETACH_ALL);
  for (auto& o : dpp->ocbs) delete o.second;
  if (dpp->own_chid) ChannelDestroy(dpp->chid);
  delete dpp;
  return 0;
}

int resmgr_attach(dispatch_t* dpp, resmgr_attr_t* attr, const char* path, enum _file_type, unsigned,
                  const resmgr_connect_funcs_t* connect_funcs, const resmgr_io_funcs_t* io_funcs, RESMGR_HANDLE_T* handle) {
  if (!dpp || !path || path[0] != '/') { errno = EINVAL; return -1; }
  std::string sock = std::string(qnxsim_root()) + path;
  std::string dir = sock.substr(0, sock.rfind('/'));
  mkdirs(dir);
  struct stat st;
  if (lstat(sock.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) { errno = EEXIST; return -1; }
    int probe = connect_to(sock, false);   // a live server already owns the name (QNX would stack the mounts)
    if (probe >= 0) { close(probe); errno = EBUSY; return -1; }
    unlink(sock.c_str());                  // stale socket from a dead process
  }
  int fd = make_listener(sock, false);
  if (fd < 0) return -1;
  std::lock_guard<std::mutex> g(dpp->mu);
  Attach a;
  a.id = dpp->next_id++;
  a.path = path;
  a.sock = sock;
  a.fd = fd;
  a.cfuncs = connect_funcs;
  a.iofuncs = io_funcs;
  a.handle = handle;
  if (attr && attr->msg_max_size > dpp->msg_max) dpp->msg_max = attr->msg_max_size;
  dpp->attaches.push_back(a);
  { std::lock_guard<std::mutex> g2(dpp->ch->mu); dpp->ch->listeners.push_back({fd, a.id}); }
  return a.id;
}

int resmgr_detach(dispatch_t* dpp, int id, unsigned) {
  if (!dpp) { errno = EINVAL; return -1; }
  std::lock_guard<std::mutex> g(dpp->mu);
  Attach* a = find_attach(dpp, id);
  if (!a || a->fd < 0) { errno = EINVAL; return -1; }
  {
    std::lock_guard<std::mutex> g2(dpp->ch->mu);
    auto& L = dpp->ch->listeners;
    L.erase(std::remove_if(L.begin(), L.end(), [&](const std::pair<int, int>& l) { return l.first == a->fd; }), L.end());
  }
  close(a->fd);
  unlink(a->sock.c_str());
  a->fd = -1;
  return 0;
}

dispatch_context_t* dispatch_context_alloc(dispatch_t* dpp) {
  if (!dpp) { errno = EINVAL; return nullptr; }
  auto* x = new CtxExt;
  memset(&x->c, 0, sizeof x->c);
  x->buf.resize(dpp->msg_max);
  x->c.resmgr_context.dpp = dpp;
  x->c.resmgr_context.msg = x->buf.data();
  x->c.resmgr_context.msg_max_size = dpp->msg_max;
  return &x->c;
}

void dispatch_context_free(dispatch_context_t* ctp) { delete ext_of(&ctp->resmgr_context); }

dispatch_context_t* dispatch_block(dispatch_context_t* ctp) {
  resmgr_context_t* c = &ctp->resmgr_context;
  dispatch_t* dpp = c->dpp;
  CtxExt* x = ext_of(c);
  for (;;) {
    Received rv;
    if (channel_receive(*dpp->ch, rv, false) != 0) return nullptr;   // errno EINTR on a signal, as on QNX
    if (rv.kind == kDisconnect) {   // client closed: drop its OCB (QNX: close_ocb), then release the fd number
      {
        std::lock_guard<std::mutex> g(dpp->mu);
        auto it = dpp->ocbs.find(rv.fd);
        if (it != dpp->ocbs.end()) { delete it->second; dpp->ocbs.erase(it); }
      }
      close(rv.fd);
      continue;
    }
    if (rv.kind == kConnect) {
      // Simulated io_open: resolve the attach the connection came through and create its OCB. Permission bits of the
      // attr are NOT checked (no credentials in the simulation); the open mode is recorded and enforced by
      // iofunc_read_verify()/iofunc_write_verify() and by handlers that test ocb->ioflag.
      std::lock_guard<std::mutex> g(dpp->mu);
      Attach* a = find_attach(dpp, rv.attach_id);
      if (!a) { MsgError(rv.fd, ENOENT); continue; }
      auto* o = new iofunc_ocb_t;
      memset(o, 0, sizeof *o);
      o->attr = a->handle;
      o->ioflag = rv.f.ioflag;
      dpp->ocbs[rv.fd] = o;
      MsgReply(rv.fd, EOK, nullptr, 0);
      continue;
    }
    memset(&c->info, 0, sizeof c->info);
    c->offset = 0;
    c->status = 0;
    if (rv.kind == kPulse) {
      c->rcvid = 0;
      fill_pulse(rv, c->msg, c->msg_max_size);
      c->size = sizeof(struct _pulse);
      return ctp;
    }
    x->payload = std::move(rv.payload);
    size_t n = std::min(x->payload.size(), x->buf.size());
    memcpy(x->buf.data(), x->payload.data(), n);
    rv.payload = x->payload;   // keep fill_info() sizes right
    fill_info(rv, &c->info, n);
    c->rcvid = rv.fd;
    c->id = rv.attach_id;
    c->size = n;
    return ctp;
  }
}

int dispatch_handler(dispatch_context_t* ctp) {
  resmgr_context_t* c = &ctp->resmgr_context;
  if (c->rcvid == 0) return 0;   // pulse: no pulse handlers are attached in this subset
  dispatch_t* dpp = c->dpp;
  iofunc_ocb_t* ocb = nullptr;
  const resmgr_io_funcs_t* io = nullptr;
  {
    std::lock_guard<std::mutex> g(dpp->mu);
    auto it = dpp->ocbs.find((int)c->rcvid);
    if (it != dpp->ocbs.end()) ocb = it->second;
    Attach* a = find_attach(dpp, c->id);
    if (a) io = a->iofuncs;
  }
  if (!ocb || !io) { MsgError(c->rcvid, EBADF); return 0; }
  if (c->size < sizeof(uint16_t)) { MsgError(c->rcvid, EBADMSG); return 0; }
  uint16_t type;
  memcpy(&type, c->msg, sizeof type);
  int r = ENOSYS;
  switch (type) {
    case _IO_READ:    if (io->read) r = io->read(c, (io_read_t*)c->msg, ocb); break;
    case _IO_READ64:  if (io->read64) r = io->read64(c, (io_read_t*)c->msg, ocb); else if (io->read) r = io->read(c, (io_read_t*)c->msg, ocb); break;
    case _IO_WRITE:   if (io->write) r = io->write(c, (io_write_t*)c->msg, ocb); break;
    case _IO_WRITE64: if (io->write64) r = io->write64(c, (io_write_t*)c->msg, ocb); else if (io->write) r = io->write(c, (io_write_t*)c->msg, ocb); break;
    case _IO_MSG:     if (io->msg) r = io->msg(c, (io_msg_t*)c->msg, ocb); break;
    default: break;
  }
  if (r == _RESMGR_NOREPLY) return 0;          // the handler replied itself (MsgReply/MsgError)
  if (r >= 0) {
    if (r == EOK) MsgReply(c->rcvid, c->status, nullptr, 0);
    else MsgError(c->rcvid, r);
    return 0;
  }
  int nparts = -r;                               // _RESMGR_NPARTS(n): reply with ctp->iov[0..n-1], status ctp->status
  if (nparts == 0) { MsgReply(c->rcvid, c->status, nullptr, 0); return 0; }
  if (nparts > 1) { MsgError(c->rcvid, ENOTSUP); return 0; }   // the stub context has one iov
  MsgReply(c->rcvid, c->status, c->iov[0].iov_base, c->iov[0].iov_len);
  return 0;
}

ssize_t resmgr_msgget(resmgr_context_t* const ctp, void* msg, size_t size, size_t offset) {
  CtxExt* x = ext_of(ctp);
  size_t start = ctp->offset + offset;
  if (start >= x->payload.size()) return 0;
  size_t n = std::min(size, x->payload.size() - start);
  memcpy(msg, x->payload.data() + start, n);
  return (ssize_t)n;
}

void iofunc_func_init(unsigned nconnect, resmgr_connect_funcs_t* connect, unsigned nio, resmgr_io_funcs_t* io) {
  // QNX installs the iofunc_*_default handlers here; the simulation leaves them empty (an unset handler -> ENOSYS).
  if (connect) { memset(connect, 0, sizeof *connect); connect->nfuncs = nconnect; }
  if (io) { memset(io, 0, sizeof *io); io->nfuncs = nio; }
}

void iofunc_attr_init_sized(iofunc_attr_t* attr, mode_t mode, iofunc_attr_t*, struct _client_info*, size_t size) {
  memset(attr, 0, std::min(size, sizeof *attr));
  attr->mode = mode;
  attr->nlink = 1;
  attr->uid = getuid();
  attr->gid = getgid();
  attr->mtime = attr->atime = attr->ctime = time(nullptr);
}

int iofunc_read_verify(resmgr_context_t*, io_read_t*, iofunc_ocb_t* ocb, int* nonblock) {
  if (nonblock) *nonblock = 0;
  return (ocb && (ocb->ioflag & _IO_FLAG_RD)) ? EOK : EBADF;
}

int iofunc_write_verify(resmgr_context_t*, io_write_t*, iofunc_ocb_t* ocb, int* nonblock) {
  if (nonblock) *nonblock = 0;
  return (ocb && (ocb->ioflag & _IO_FLAG_WR)) ? EOK : EBADF;
}

// open(): a path attached by a (simulated) resource manager -> connection + CONNECT; anything else -> the real open().
int qnxsim_open(const char* path, int oflag, ...) {
  mode_t mode = 0;
  if (oflag & O_CREAT) { va_list ap; va_start(ap, oflag); mode = (mode_t)va_arg(ap, int); va_end(ap); }
  if (path && path[0] == '/') {
    std::string sock = std::string(qnxsim_root()) + path;
    struct stat st;
    if (stat(sock.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
      int fd = connect_to(sock, false);
      if (fd < 0) { if (errno == ECONNREFUSED) errno = ENOENT; return -1; }
      Frame f;
      f.kind = kConnect;
      int acc = oflag & O_ACCMODE;
      f.ioflag = (acc == O_RDONLY ? _IO_FLAG_RD : acc == O_WRONLY ? _IO_FLAG_WR : (_IO_FLAG_RD | _IO_FLAG_WR));
      Frame r;
      std::vector<char> payload;
      if (!send_frame(fd, f, nullptr, 0) || recv_frame(fd, r, payload) <= 0) { close(fd); errno = ENOENT; return -1; }
      if (r.err) { close(fd); errno = r.err; return -1; }
      return fd;
    }
  }
  return open(path, oflag, mode);
}

}  // extern "C"
