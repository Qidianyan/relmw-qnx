// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// QNX only: register a relmw process with the High Availability Manager so that it is restarted automatically if
// it dies (CONDDEATH -> restart action). Requires `ham` to be running on the target.
//   relmw_ham_setup [--name ENTITY] "<full command line>"        (ENTITY default: relmw_supervisor)
//   relmw_ham_setup "/usr/bin/relmw_supervisor --episodes 0 --library /data/relmw/failure_library.txt"
//   relmw_ham_setup --name relmw_robotd "/usr/bin/relmw_robotd --path /dev/robot0"
#ifdef __QNX__
#include <errno.h>
#include <ha/ham.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
  const char* name = "relmw_supervisor";
  int a = 1;
  if (argc > 2 && strcmp(argv[1], "--name") == 0) { name = argv[2]; a = 3; }
  const char* line = argc > a ? argv[a] : "/usr/bin/relmw_supervisor --episodes 0";
  if (ham_connect(0) == -1) { fprintf(stderr, "ham_connect: %s (is `ham` running?)\n", strerror(errno)); return 1; }
  // pid <= 0: HAM starts the process itself and monitors it.
  ham_entity_t* e = ham_attach(name, 0, -1, line, 0);
  if (!e) { fprintf(stderr, "ham_attach: %s\n", strerror(errno)); ham_disconnect(0); return 1; }
  ham_condition_t* c = ham_condition(e, CONDDEATH, "death", HREARMAFTERRESTART);
  if (!c) { fprintf(stderr, "ham_condition: %s\n", strerror(errno)); ham_disconnect(0); return 1; }
  if (!ham_action_restart(c, "restart", line, HREARMAFTERRESTART)) {
    fprintf(stderr, "ham_action_restart: %s\n", strerror(errno)); ham_disconnect(0); return 1;
  }
  printf("%s attached to HAM (restart on death): %s\n", name, line);
  ham_disconnect(0);
  return 0;
}
#else
#include <cstdio>
int main() { std::puts("relmw_ham_setup is QNX-only (High Availability Manager)."); return 0; }
#endif
