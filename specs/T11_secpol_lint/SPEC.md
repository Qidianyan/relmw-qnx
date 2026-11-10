# T11: host-side lint for the QNX security policy (checklist C9)

Working directory: repository root. Python 3 standard library only. Read first: config/secpol/relmw_secpol.txt,
docs/secpol.md, src/platform/qnx/qnx_resmgr.cpp (resmgr_attach paths built from the mount point + "/status" etc.),
apps/relmw_supervisor.cpp (default mount "/dev/relmw"), CMakeLists.txt, tools/test_make_dashboard.py (test style).
Do NOT modify config/secpol/relmw_secpol.txt except to fix a real lint finding (report it). Do not touch C++ sources.

The real compiler (`secpolcompile`, QNX SDP 8.0, host tool) is not installed here. This lint is a host-side
approximation of the QNX 8.0 "Security policy language" plus project-specific least-privilege rules. It must never claim
the policy compiles.

## `tools/secpol_lint.py POLICY [--repo ROOT] [--extern NAME...]` (exit 0 clean, 1 findings, 2 usage)
Parsing (QNX 8.0 language subset): `#` comments to end of line; statements end with `;`; whitespace insignificant;
`{ ... }` sets. Recognise statements: `type NAME[, attr...];`, `attribute NAME;`, `allow SRC TGT:ability { ... };` /
`allow SRC TGT:ability NAME;`, `allow SRCSET TGTSET:channel connect;` (SRC/TGT may be a name or `{ a b }`),
`allow_attach TYPE PATH [CHANNEL_TYPE];` and `allow_attach TYPE { PATH... };` (PATH may have a `ftype:` / `ftype+:`
prefix), `allow_link`, `derive_type`, `default_spawn_type`, `mount_point`, `ability NAME;`, `range NAME;`, `class NAME { ... };`.
Anything else -> finding "unknown statement" with line number. Unbalanced braces / missing `;` -> finding with line number.

Checks (each finding: `POLICY:LINE: <rule-id>: message`):
- R1 every type used (allow/allow_attach/derive...) is declared with `type`, is `self`, or is extern. Externs come from
  `--extern` and from a comment line `# extern: a b c` in the policy.
- R2 ability names must be from the QNX 8.0 Abilities Reference (transcribe this list into the tool, with the comment
  "QNX SDP 8.0 System Security Guide, Abilities Reference, read 2026-11-10"): able_create able_priv channel_connect
  child_newapp chroot clockperiod clockset connection cpumode default_timer_tolerance eventtrace getid hyp interrupt io
  keydata mac_policy map_fixed mem_add mem_global mem_lock mem_peer mem_phys mem_special mountifs msg_queue pathspace
  path_trust pgrp power priority prot_exec prot_write_and_exec public_channel qvm reboot rlimit rlimit_peer rsrcdbmgr
  rsrcdbmgr_unrestricted runstate runstate_burst sched_partition sched_partition_join session setgid setuid settypeid
  sigev_thread signal spawn spawn_setgid spawn_setuid srandom timer trace untrusted_exec v86 wait xprocess_able
  xprocess_debug xprocess_mem_read xprocess_query, plus options `nonroot`, `unlock`, `noinherit`, `gain_priv`, plus custom
  abilities declared with `ability NAME;`. Ranges after `:` are allowed (`setuid:4-6,23`). Matching is case-insensitive.
- R3 least privilege (project rule): for types declared in this policy, forbid `unlock`, `able_priv`, `io`, `interrupt`,
  `mem_phys`, `mem_add`, `mem_peer`, `mac_policy`, `xprocess_*`, `settypeid`, `setuid`, `spawn_setuid`, `hyp`, and any
  `priority` without a range; forbid `allow_attach` with `/...`, `*` anywhere in a path, or a `*:` prefix.
- R4 coverage: every path the code registers must be allowed for the right type. Derive the paths by scanning the repo:
  for qnx_resmgr.cpp, take the suffixes in `resmgr_attach(... (mount_ + "/status") ...)` combined with the supervisor's
  default mount from apps/relmw_supervisor.cpp (`std::string mount = "/dev/relmw";` inside `#ifdef __QNX__`) ->
  relmw_supervisor_t; any `resmgr_attach` path or default `--path` literal starting with `/dev/` in
  src/platform/qnx/qnx_robot_link.cpp or apps/relmw_robotd.cpp (if those files exist) -> relmw_robotd_t. Missing
  coverage -> finding. Paths allowed in the policy but never registered by the code -> finding R5 (over-privilege).
- R6 every declared type is used by at least one rule.
Print `secpol_lint: OK (<n> statements, <t> types)` when clean.

## Tests `tools/test_secpol_lint.py` (unittest; register in CMakeLists.txt as `test_secpol_lint` inside `if(Python3_FOUND)`,
running `tools/secpol_lint.py config/secpol/relmw_secpol.txt --repo ${CMAKE_CURRENT_SOURCE_DIR}` must pass as part of it)
- the shipped policy is clean;
- one negative case per rule R1..R6 plus: missing `;`, unbalanced `{`, unknown statement, unknown ability, ability case
  insensitivity, set syntax `allow { a b } c:channel connect;`, `ftype:` prefix parsing, `# extern:` handling, ranges.
- R4: a temp repo copy where a new `resmgr_attach(... (mount_ + "/debug") ...)` is added -> finding.
Runtime < 5 s.

## Acceptance
```
python3 tools/secpol_lint.py config/secpol/relmw_secpol.txt --repo . ; python3 tools/test_secpol_lint.py -v 2>&1 | tail -3
rm -rf /tmp/relmw_t11 && cmake -S . -B /tmp/relmw_t11 -G Ninja >/dev/null && cmake --build /tmp/relmw_t11 >/dev/null && ctest --test-dir /tmp/relmw_t11 -R secpol --output-on-failure | tail -3
```
SPDX MIT header + "Copyright (c) 2026 elidot" in new files. No git commits. Budget: 15 minutes.

## Final reply
Files + one line each, test output tail, any finding against the shipped policy.
