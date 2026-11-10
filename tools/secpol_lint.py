#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
"""Host-side lint for the relmw QNX security policy (checklist C9).

Host-side approximation of the QNX 8.0 "Security policy language" (System
Security Guide) plus the project's least-privilege rules.  This is NOT
secpolcompile: it never claims the policy compiles; it only flags likely
mistakes on the host before the real compiler (QNX SDP 8.0 host tool) runs.

Usage:
    secpol_lint.py POLICY [--repo ROOT] [--extern NAME...]

Exit codes: 0 = clean, 1 = findings, 2 = usage error.

Checks:
  R1 every type used (allow/allow_attach/derive...) is declared with `type`,
     is `self`, or is extern (--extern and `# extern: a b c` comment lines).
  R2 ability names must be from the QNX 8.0 Abilities Reference (plus the
     options nonroot/unlock/noinherit/gain_priv and custom `ability NAME;`);
     ranges after ':' allowed; matching is case-insensitive.
  R3 least privilege: for types declared in this policy, forbid a fixed list
     of abilities and any `priority` without a range; forbid allow_attach
     paths with '/...', '*', or a '*:' prefix.
  R4 coverage: every path the code registers (resmgr_attach suffixes + the
     supervisor's default mount; /dev/... literals in the robotd sources)
     must be allowed for the right type.
  R5 over-privilege: paths allowed in the policy but never registered by the
     code (only judged for types whose code paths could be derived).
  R6 every declared type is used by at least one rule.
"""

import argparse
import os
import re
import sys

# QNX SDP 8.0 System Security Guide, Abilities Reference, read 2026-11-10 (64 names, extracted from the table on
# https://www.qnx.com/developers/docs/8.0/com.qnx.doc.security.system/topic/manual/abilities_reference.html).
QNX_ABILITIES = frozenset(
    """able_create able_priv channel_connect child_newapp chroot clockset confset connection cpumode
    default_timer_tolerance event fork getid high_resolution_timer hyp interrupt io mac_policy map_fixed mem_add
    mem_lock mem_peer mem_phys mem_special mountifs msg_queue pathspace path_trust pgrp power priority privreg
    prot_exec prot_write_and_exec public_channel rconstraint reboot rlimit rlimit_peer rsrcdbmgr runstate
    runstate_burst sandbox schedule server_monitor session setgid settypeid setuid sigev_thread signal spawn
    spawn_setgid spawn_setuid srandom swap trace umask untrusted_exec wait xprocess_able xprocess_debug
    xprocess_mem_read xprocess_query""".split())
assert len(QNX_ABILITIES) == 64

# R2: valid non-ability options inside an ability list.
ABILITY_OPTIONS = frozenset(("nonroot", "unlock", "noinherit", "gain_priv"))

# R3: project least-privilege rule, applies to types declared in this policy.
R3_FORBIDDEN = frozenset(
    ("unlock", "able_priv", "io", "interrupt", "mem_phys", "mem_add",
     "mem_peer", "mac_policy", "settypeid", "setuid", "spawn_setuid", "hyp"))
R3_FORBIDDEN_PREFIX = "xprocess_"

IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
EXTERN_COMMENT_RE = re.compile(r"^[ \t]*#[ \t]*extern:[ \t]*(.*)$")
RESMGR_CALL_RE = re.compile(r"resmgr_attach\((.*?)\);", re.S)
MOUNT_SUFFIX_RE = re.compile(r'mount_\s*\+\s*"(/[^"]*)"')
DEFAULT_MOUNT_RE = re.compile(r'std::string\s+mount\s*=\s*"([^"]+)"')
DEV_LITERAL_RE = re.compile(r'"/dev/[^"]*"')


class Bad(Exception):
    """Statement does not match any known shape."""


def strip_comments(text):
    """'#' comments run to end of line; line numbering is preserved."""
    out = []
    for line in text.splitlines():
        i = line.find("#")
        out.append(line if i < 0 else line[:i])
    return "\n".join(out)


def collect_externs(text, cli_externs):
    ext = set()
    for chunk in cli_externs:
        ext.update(chunk.split())
    for line in text.splitlines():
        m = EXTERN_COMMENT_RE.match(line)
        if m:
            ext.update(m.group(1).split())
    return ext


def tokenize(text):
    """Words plus the structural tokens { } ; ( ':' and ',' stay inside words)."""
    toks = []
    line = 1
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r\f\v":
            i += 1
        elif c in "{};":
            toks.append((c, line))
            i += 1
        else:
            j = i
            while j < n and text[j] not in " \t\r\f\v\n{};":
                j += 1
            toks.append((text[i:j], line))
            i = j
    return toks


def split_statements(toks):
    """Group tokens into ';'-terminated statements; report brace/';' problems."""
    stmts = []
    findings = []
    i = 0
    n = len(toks)
    while i < n:
        start_line = toks[i][1]
        depth = 0
        body = []
        j = i
        while j < n:
            word, wline = toks[j]
            if word == "{":
                depth += 1
                body.append((word, wline))
            elif word == "}":
                if depth == 0:
                    findings.append((wline, "PARSE", "unbalanced '}'"))
                else:
                    depth -= 1
                    body.append((word, wline))
            elif word == ";" and depth == 0:
                break
            else:
                body.append((word, wline))
            j += 1
        else:
            # EOF reached without the closing ';'
            if depth > 0:
                findings.append((start_line, "PARSE", "unbalanced '{'"))
            if body:
                findings.append((start_line, "PARSE", "missing ';'"))
            break
        stmts.append((body, start_line))
        i = j + 1
    return stmts, findings


def parse_set(words, i):
    """NAME or { NAME NAME ... } -> ( [(name, line), ...], index_after )."""
    if words[i][0] == "{":
        j = i + 1
        items = []
        while j < len(words) and words[j][0] != "}":
            items.append((words[j][0].strip(","), words[j][1]))
            j += 1
        if j >= len(words):
            raise Bad()
        return items, j + 1
    return [(words[i][0].strip(","), words[i][1])], i + 1


def norm_path(word):
    """Strip an ftype: / ftype+: prefix from an allow_attach path."""
    if word.startswith("ftype+:"):
        return word[len("ftype+:"):]
    if word.startswith("ftype:"):
        return word[len("ftype:"):]
    return word


def extract_default_mount(text):
    """The `std::string mount = ...;` default inside `#ifdef __QNX__`."""
    if not text:
        return None
    in_qnx = False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#ifdef __QNX__"):
            in_qnx = True
            continue
        if in_qnx and (s.startswith("#else") or s.startswith("#endif")):
            in_qnx = False
            continue
        if in_qnx:
            m = DEFAULT_MOUNT_RE.search(line)
            if m:
                return m.group(1)
    return None


def derive_code_paths(repo):
    """Paths the code registers, per policy type (R4/R5 evidence)."""
    out = {"relmw_supervisor_t": set(), "relmw_robotd_t": set()}
    resmgr = os.path.join(repo, "src", "platform", "qnx", "qnx_resmgr.cpp")
    if os.path.exists(resmgr):
        with open(resmgr, encoding="utf-8") as fh:
            text = fh.read()
        suffixes = set()
        for call in RESMGR_CALL_RE.finditer(text):
            for m in MOUNT_SUFFIX_RE.finditer(call.group(1)):
                suffixes.add(m.group(1))
        sup = os.path.join(repo, "apps", "relmw_supervisor.cpp")
        mount = None
        if os.path.exists(sup):
            with open(sup, encoding="utf-8") as fh:
                mount = extract_default_mount(fh.read())
        if mount and suffixes:
            out["relmw_supervisor_t"] = {mount + s for s in suffixes}
    for rel in ("src/platform/qnx/qnx_robot_link.cpp", "apps/relmw_robotd.cpp"):
        path = os.path.join(repo, rel)
        if os.path.exists(path):
            with open(path, encoding="utf-8") as fh:
                for lit in DEV_LITERAL_RE.findall(fh.read()):
                    out["relmw_robotd_t"].add(lit[1:-1])
    return out


def lint(text, repo, externs):
    """Return (findings, n_statements, n_types). findings: (line, rule, msg)."""
    findings = []
    stmts, parse_findings = split_statements(tokenize(strip_comments(text)))
    findings.extend(parse_findings)

    declared = {}          # type name -> declaration line
    used = set()
    custom_abilities = set()
    attach = []            # (type, normalized path, line)

    def check_type(name, line):
        if name.lower() == "self":
            return
        if name not in declared and name not in externs:
            findings.append(
                (line, "R1", "type '%s' is not declared with type, self, or extern" % name))

    def check_ability(word, line, local):
        name, sep, rng = word.strip(",").partition(":")
        if not name:
            findings.append((line, "R2", "empty ability name"))
            return
        lname = name.lower()
        if (lname not in QNX_ABILITIES and lname not in ABILITY_OPTIONS
                and lname not in custom_abilities):
            findings.append((line, "R2", "unknown ability '%s'" % name))
            return
        if not local:
            return
        if lname in R3_FORBIDDEN or lname.startswith(R3_FORBIDDEN_PREFIX):
            findings.append(
                (line, "R3", "ability '%s' is forbidden for policy-declared types (least privilege)" % name))
        elif lname == "priority" and not rng:
            findings.append((line, "R3", "priority without a range is forbidden"))

    for body, start_line in stmts:
        if not body:
            continue
        kw = body[0][0]
        words = body[1:]
        if kw == "type":
            args = [w.strip(",") for w, _ in words]
            args = [a for a in args if a]
            if not args or not all(IDENT_RE.match(a) for a in args):
                findings.append((start_line, "PARSE", "unknown statement: malformed 'type'"))
            else:
                declared.setdefault(args[0], start_line)
        elif kw in ("attribute", "range"):
            args = [w.strip(",") for w, _ in words]
            if len(args) != 1 or not IDENT_RE.match(args[0]):
                findings.append((start_line, "PARSE", "unknown statement: malformed '%s'" % kw))
        elif kw == "ability":
            args = [w.strip(",") for w, _ in words]
            if len(args) != 1 or not IDENT_RE.match(args[0]):
                findings.append((start_line, "PARSE", "unknown statement: malformed 'ability'"))
            else:
                custom_abilities.add(args[0].lower())
        elif kw == "class":
            ws = [w for w, _ in words]
            if len(ws) < 3 or ws[1] != "{" or ws[-1] != "}":
                findings.append((start_line, "PARSE", "unknown statement: malformed 'class'"))
        elif kw in ("allow", "allow_link"):
            try:
                src, i = parse_set(words, 0)
                if words[i][0] == "{":
                    tgt, i = parse_set(words, i)
                    if not words[i][0].startswith(":"):
                        raise Bad()
                    cls = words[i][0][1:]
                    i += 1
                else:
                    tgt_name, sep, cls = words[i][0].partition(":")
                    if not sep:
                        raise Bad()
                    tgt = [(tgt_name, words[i][1])]
                    i += 1
                rest = [w for w, _ in words[i:]]
                if cls == "channel":
                    if rest != ["connect"]:
                        raise Bad()
                    abils = []
                elif cls == "ability":
                    if rest and rest[0] == "{":
                        abils, j = parse_set(words, i)
                        if j != len(words):
                            raise Bad()
                    else:
                        if len(rest) != 1:
                            raise Bad()
                        abils = list(words[i:])
                else:
                    raise Bad()
            except (Bad, IndexError):
                findings.append((start_line, "PARSE", "unknown statement '%s'" % kw))
                continue
            for name, ln in src + tgt:
                check_type(name, ln)
                used.add(name)
            local = any(n in declared for n, _ in src)
            for name, ln in abils:
                check_ability(name, ln, local)
        elif kw == "allow_attach":
            if len(words) < 2:
                findings.append((start_line, "PARSE", "unknown statement 'allow_attach'"))
                continue
            tname, tline = words[0]
            rest = words[1:]
            if rest[0][0] == "{":
                if rest[-1][0] != "}":
                    findings.append((start_line, "PARSE", "unknown statement 'allow_attach'"))
                    continue
                path_words = rest[1:-1]
            else:
                if len(rest) > 2:  # PATH [CHANNEL_TYPE]
                    findings.append((start_line, "PARSE", "unknown statement 'allow_attach'"))
                    continue
                path_words = rest[:1]
            check_type(tname, tline)
            used.add(tname)
            local = tname in declared
            for pw, pline in path_words:
                attach.append((tname, norm_path(pw), pline))
                if local:
                    if pw.startswith("*:"):
                        findings.append((pline, "R3", "allow_attach path '%s' uses forbidden '*:' prefix" % pw))
                    elif "*" in pw:
                        findings.append((pline, "R3", "allow_attach path '%s' contains '*'" % pw))
                    if "..." in pw:
                        findings.append((pline, "R3", "allow_attach path '%s' contains '...'" % pw))
        elif kw in ("derive_type", "default_spawn_type"):
            args = [(w.strip(","), ln) for w, ln in words]
            if not args or not all(IDENT_RE.match(a) for a, _ in args):
                findings.append((start_line, "PARSE", "unknown statement: malformed '%s'" % kw))
            else:
                for name, ln in args:
                    check_type(name, ln)
                    used.add(name)
        elif kw == "mount_point":
            pass  # path namespace statement; no type/ability semantics checked
        else:
            findings.append((start_line, "PARSE", "unknown statement '%s'" % kw))

    # R4 coverage + R5 over-privilege from the code in --repo.
    code_paths = derive_code_paths(repo)
    for tname, expected in sorted(code_paths.items()):
        if not expected:
            continue  # no code evidence for this type; R4/R5 not judgeable
        allowed = {p for t, p, _ in attach if t == tname}
        decl_line = declared.get(tname)
        for p in sorted(expected - allowed):
            findings.append((decl_line or 1, "R4",
                             "path '%s' is registered by the code but not allowed for %s" % (p, tname)))
        for t, p, ln in attach:
            if t == tname and p not in expected:
                findings.append((ln, "R5",
                                 "path '%s' is allowed for %s but never registered by the code" % (p, tname)))

    # R6: every declared type is used by at least one rule.
    for tname, ln in sorted(declared.items()):
        if tname not in used:
            findings.append((ln, "R6", "type '%s' is declared but never used by a rule" % tname))

    findings.sort(key=lambda f: (f[0], f[1]))
    return findings, len(stmts), len(declared)


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="secpol_lint.py",
        description="Host-side lint for a QNX 8.0 security policy "
                    "(approximation of the policy language + project least-privilege rules; "
                    "not secpolcompile, never claims the policy compiles).")
    ap.add_argument("policy", help="path to the security policy file")
    ap.add_argument("--repo", default=".", help="repository root used to derive registered paths (R4/R5)")
    ap.add_argument("--extern", action="append", nargs="+", default=[],
                    help="extern type name(s) provided by the image policy")
    args = ap.parse_args(argv)

    if not os.path.isfile(args.policy):
        sys.stderr.write("secpol_lint: cannot read policy: %s\n" % args.policy)
        return 2
    try:
        with open(args.policy, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        sys.stderr.write("secpol_lint: cannot read policy: %s\n" % exc)
        return 2

    externs = collect_externs(text, [n for chunk in args.extern for n in chunk])
    findings, nstmt, ntypes = lint(text, args.repo, externs)
    for ln, rule, msg in findings:
        print("%s:%d: %s: %s" % (args.policy, ln, rule, msg))
    if findings:
        return 1
    print("secpol_lint: OK (%d statements, %d types)" % (nstmt, ntypes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
