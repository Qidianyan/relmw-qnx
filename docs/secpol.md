<!-- SPDX-License-Identifier: MIT -->
# Least-privilege security policy (QNX SDP 8.0)

**Status: draft.** `config/secpol/relmw_secpol.txt` is written from the QNX SDP 8.0 System Security Guide and Utilities
Reference. It has not been compiled with `secpolcompile` or pushed to a target yet. `tools/secpol_lint.py` (run by ctest)
checks it on the host against the rules below; the real check is the validation sequence at the end of this page.

## Why a policy
Out of the box, a QNX process running as root can register any path, connect to any channel and raise its own priority.
On a robot cell we want the opposite: each process can do only its own job.

| Process | Type | Runs as | May register | May connect to | Abilities |
|---|---|---|---|---|---|
| `relmw_supervisor` | `relmw_supervisor_t` | uid 1001 (non-root) | `/dev/relmw/{status,events,ctl}` | robot driver, slogger2, HAM | `pathspace`, `public_channel` |
| `relmw_robotd` | `relmw_robotd_t` | uid 1001 | `/dev/robot0` | slogger2 | `pathspace`, `public_channel` |
| operator tools (`relmwctl`, `cat`, `echo`) | `relmw_operator_t` | uid 1002 | nothing | supervisor, robot driver | none |

Some things are deliberately not granted:
- **`priority`**: the control thread runs at priority 40, below the unprivileged maximum of 63 (8.0 Abilities Reference,
  `on -p`). Add `priority:64-<n>` only if you need `--prio` above 63.
- **`io`, `interrupt`, `mem_phys`**: these are not needed. The robot driver here is a simulator. A real driver would get
  exactly its device's `mem_phys` and `interrupt` ranges, and nothing else.
- **`spawn`, `setuid`, `settypeid`, `signal`, `xprocess_*`, `able_priv`, `unlock`**: none of them are needed. Process restarts
  and killing a hung process are done by HAM, not by us.
- **Runmask**: `pthread_cluster_set()` needs no ability.

## How the rules map to the code
| Rule | Code |
|---|---|
| `allow_attach relmw_supervisor_t { /dev/relmw/status ... }` | `src/platform/qnx/qnx_resmgr.cpp` `ControlSurface::start()`: three `resmgr_attach()` calls |
| `allow_attach relmw_robotd_t /dev/robot0` | `src/platform/qnx/qnx_robot_link.cpp` `robot_serve()` |
| `pathspace` | `resmgr_attach()` (8.0: "register names in the path space by calling resmgr_attach()") |
| `public_channel` | `dispatch_create()` / `dispatch_create_channel()` create a public channel |
| `relmw_supervisor_t relmw_robotd_t:channel connect` | `open("/dev/robot0")` then `MsgSend()` in `robot_connect()` |
| `relmw_operator_t relmw_supervisor_t:channel connect` | `cat /dev/relmw/status`, `echo ... > /dev/relmw/ctl` |

Channels take the type of the process that owns them. Once a policy is installed, a process can connect only to channels it
is explicitly allowed to. So the `0666` permissions on `/dev/relmw/ctl` are only a second line of defence. The first is that
nothing except `relmw_operator_t` can even open the supervisor's channel.

## Things to adjust for a given image
- `slogger2_t` and `ham_t` are the types the image's own policy gives to slogger2 and HAM. Use the names from that policy, or
  from `secpolgenerate` output.
- **HAM restart**: HAM respawns the supervisor with the command line registered by `relmw_ham_setup`. For the new process to
  get `relmw_supervisor_t`, do one of two things. Either prefix that command with `on -T relmw_supervisor_t -u 1001:1001`
  (which needs `settypeid:relmw_supervisor_t` for HAM's type), or go through `secpollaunch`. The Security Guide recommends
  `secpollaunch` because it avoids a `gain_priv` rule.

## Validation (needs SDP 8.0 and the QEMU target, in this order)
1. Compile on the host: `secpolcompile -m -o /tmp/relmw.bin config/secpol/relmw_secpol.txt` (`scripts/qnx_first_run.sh` does
   this when `secpolcompile` is on PATH).
2. Learn what is actually used: boot with `secpolgenerate`, run `scripts/target/relmw_target_run.sh`, then fetch
   `/dev/secpolgenerate/policy` with `ssh root@<ip> cat` (not `scp`). Diff it against this file. Anything extra must be
   explained or removed.
3. Install the policy, start the three processes with `on -T ... -u ...`, and rerun the target script. Then run the
   negative checks:
   - An untyped shell cannot open `/dev/relmw/ctl`.
   - `relmw_supervisor --prio 70` fails with `EPERM`.
   - `relmw_robotd --path /dev/robot1` fails in `resmgr_attach()`.
4. Only then compile without `-m` (immutable).

【待 QNX 实测】 Output of steps 1–3.
