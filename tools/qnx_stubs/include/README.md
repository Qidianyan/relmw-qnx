# QNX 8.0 API stubs for host-side syntax checking (NOT a QNX SDK)

Declarations transcribed by hand from the public QNX SDP 8.0 documentation (C Library Reference, Writing a Resource
Manager, High Availability Framework Developer's Guide; read 2026-11-10). Only what `src/platform/qnx/` and
`apps/relmw_ham_setup.cpp` use. They exist so that `scripts/qnx_syntax_check.sh` can run `g++ -fsyntax-only -D__QNX__`
on the QNX layer before the real SDP is installed. Limits: struct layouts are abbreviated, macros only type-check their
arguments, and nothing here is linked or executed. The real check is the first `q++` build (scripts/qnx_first_run.sh).
