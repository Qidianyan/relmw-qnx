#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
# Host-side static check of the QNX-only code before an SDP is available:
#   g++ -fsyntax-only -D__QNX__ against hand-written QNX 8.0 API stubs (tools/qnx_stubs/include, see its README).
# Catches wrong argument counts/types, missing includes, typos in QNX identifiers. It does NOT prove the code links or
# runs on QNX; the first real q++ build is scripts/qnx_first_run.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-g++}"
FLAGS=(-std=c++17 -fsyntax-only -Wall -Wextra -Werror -D__QNX__ -isystem tools/qnx_stubs/include -Iinclude)
FILES=(src/platform/qnx/qnx_timer.cpp src/platform/qnx/qnx_log.cpp src/platform/qnx/qnx_resmgr.cpp
       src/platform/qnx/qnx_robot_link.cpp src/platform/latency.cpp src/core/robot_link.cpp
       src/core/safety_shield.cpp src/core/nn_policy.cpp src/core/telemetry_bridge.cpp
       apps/relmw_supervisor.cpp apps/relmw_ham_setup.cpp apps/relmw_robotd.cpp)
rc=0
for f in "${FILES[@]}"; do
  if "$CXX" "${FLAGS[@]}" "$f"; then echo "OK    $f"; else echo "FAIL  $f"; rc=1; fi
done
# Every QNX source must actually be guarded (a missing #ifdef would break the Linux build silently skipping nothing).
for f in src/platform/qnx/*.cpp; do grep -q '^#ifdef __QNX__' "$f" || { echo "FAIL  $f lacks #ifdef __QNX__"; rc=1; }; done
# __QNXNTO__ is discontinued in QNX 8.0; make sure nobody relies on it.
if grep -rn '__QNXNTO__' src apps include >/dev/null; then echo "FAIL  __QNXNTO__ used (removed in 8.0)"; rc=1; fi
# Self-test: the stubs must REJECT known-wrong calls, otherwise a green result above means nothing.
neg() {  # $1 = description, $2 = code
  if printf '%s\n' "$2" | "$CXX" -std=c++17 -fsyntax-only -D__QNX__ -isystem tools/qnx_stubs/include -x c++ - 2>/dev/null; then
    echo "FAIL  stub self-test accepted: $1"; rc=1
  else echo "OK    stub rejects: $1"; fi
}
neg "ham_attach() with 4 args" '#include <ha/ham.h>
int f(){ return ham_attach("x", 0, "line", 0) != 0; }'
neg "resmgr_attach() missing handle" '#include <sys/dispatch.h>
int f(dispatch_t* d, resmgr_connect_funcs_t* c, resmgr_io_funcs_t* i){ return resmgr_attach(d, 0, "/dev/x", _FTYPE_ANY, 0, c, i); }'
neg "slog2_register() with 2 args" '#include <sys/slog2.h>
int f(slog2_buffer_set_config_t* c, slog2_buffer_t* h){ return slog2_register(c, h); }'
neg "unknown sched_param member" '#include <sched.h>
int f(){ struct sched_param p; return p.sched_ss_budget; }'
neg "MsgReceivePulse() with 3 args" '#include <sys/neutrino.h>
int f(int c, struct _pulse* p){ return MsgReceivePulse(c, p, sizeof *p); }'
neg "MsgReply() without status" '#include <sys/neutrino.h>
int f(rcvid_t r, const void* m){ return MsgReply(r, m, 8); }'
neg "io_funcs.msg with a devctl-style handler" '#include <sys/iofunc.h>
int h(resmgr_context_t*, io_read_t*, iofunc_ocb_t*); void f(resmgr_io_funcs_t* io){ io->msg = h; }'
neg "pthread_cluster_set() without tid" '#include <pthread.h>
int f(){ return pthread_cluster_set("_cpu-0"); }'
neg "pthread_cluster_get_runmask() with unsigned*" '#include <pthread.h>
int f(unsigned* m){ return pthread_cluster_get_runmask("_all", m); }'
[ $rc -eq 0 ] && echo "qnx_syntax_check: all QNX translation units pass -fsyntax-only against 8.0 stubs"
exit $rc
