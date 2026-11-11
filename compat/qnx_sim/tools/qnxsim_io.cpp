// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX API simulation on Linux, not QNX.
// qnxsim_io: what `cat PATH` and `echo TEXT > PATH` do on QNX, for paths served by a simulated resource manager:
//   qnxsim_io read PATH         open(O_RDONLY) + _IO_READ messages until 0 bytes; prints the data
//   qnxsim_io write PATH TEXT   open(O_WRONLY) + one _IO_WRITE message carrying TEXT and a newline
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/iofunc.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <unistd.h>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3 || (strcmp(argv[1], "read") && strcmp(argv[1], "write")) || (!strcmp(argv[1], "write") && argc < 4)) {
    fprintf(stderr, "usage: qnxsim_io read PATH | qnxsim_io write PATH TEXT   (QNX API simulation on Linux, not QNX)\n");
    return 2;
  }
  bool rd = !strcmp(argv[1], "read");
  int fd = open(argv[2], rd ? O_RDONLY : O_WRONLY);
  if (fd < 0) { fprintf(stderr, "qnxsim_io: open(%s): %s\n", argv[2], strerror(errno)); return 1; }
  if (rd) {
    char buf[4096];
    for (;;) {
      io_read_t m;
      memset(&m, 0, sizeof m);
      m.i.type = _IO_READ;
      m.i.combine_len = sizeof m.i;
      m.i.nbytes = sizeof buf;
      long n = MsgSend(fd, &m, sizeof m.i, buf, sizeof buf);
      if (n < 0) { fprintf(stderr, "qnxsim_io: read: %s\n", strerror(errno)); close(fd); return 1; }
      if (n == 0) break;
      fwrite(buf, 1, (size_t)n, stdout);
    }
  } else {
    std::string text = std::string(argv[3]) + "\n";
    std::vector<char> msg(sizeof(io_write_t) + text.size());
    io_write_t h;
    memset(&h, 0, sizeof h);
    h.i.type = _IO_WRITE;
    h.i.combine_len = sizeof h.i;
    h.i.nbytes = (uint32_t)text.size();
    memcpy(msg.data(), &h, sizeof h);
    memcpy(msg.data() + sizeof h, text.data(), text.size());
    long n = MsgSend(fd, msg.data(), msg.size(), nullptr, 0);
    if (n < 0) { fprintf(stderr, "qnxsim_io: write: %s\n", strerror(errno)); close(fd); return 1; }
  }
  close(fd);
  return 0;
}
