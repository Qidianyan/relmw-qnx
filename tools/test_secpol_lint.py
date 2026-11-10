#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
"""Tests for tools/secpol_lint.py (registered in ctest as test_secpol_lint).

Runs the real linter against the shipped policy (must be clean, using the real
repository for the R4/R5 code scan) and against synthetic policies: one
negative case per rule R1..R6 plus the parser cases (missing ';', unbalanced
'{', unknown statement), ability handling (unknown, case-insensitive, ranges),
set syntax, ftype: prefix parsing and '# extern:' handling.
"""

import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "secpol_lint.py")
POLICY = os.path.join(ROOT, "config", "secpol", "relmw_secpol.txt")

# Minimal repo that reproduces the code evidence the linter scans for R4/R5.
RESMGR_CPP = """\
#include <string>
void f(std::string mount_) {
  resmgr_attach(d, &r, (mount_ + "/status").c_str(), 0, 0, &c, &i, &a);
  resmgr_attach(d, &r, (mount_ + "/events").c_str(), 0, 0, &c, &i, &a);
  resmgr_attach(d, &r, (mount_ + "/ctl").c_str(), 0, 0, &c, &i, &a);
}
"""
RESMGR_CPP_DEBUG = RESMGR_CPP.replace(
    '  resmgr_attach(d, &r, (mount_ + "/ctl").c_str(), 0, 0, &c, &i, &a);\n',
    '  resmgr_attach(d, &r, (mount_ + "/ctl").c_str(), 0, 0, &c, &i, &a);\n'
    '  resmgr_attach(d, &r, (mount_ + "/debug").c_str(), 0, 0, &c, &i, &a);\n')
SUPERVISOR_CPP = """\
#ifdef __QNX__
  std::string mount = "/dev/relmw";
#else
  std::string mount = "/tmp/relmw";
#endif
"""

CLEAN_POLICY = """\
type relmw_supervisor_t;
allow_attach relmw_supervisor_t {
    /dev/relmw/status
    /dev/relmw/events
    /dev/relmw/ctl
};
allow relmw_supervisor_t self:ability { pathspace public_channel };
"""


class SecpolLintTest(unittest.TestCase):
    def run_lint(self, policy_text, repo=None, externs=()):
        with tempfile.TemporaryDirectory() as td:
            pol = os.path.join(td, "policy.txt")
            with open(pol, "w", encoding="utf-8") as fh:
                fh.write(policy_text)
            if repo is None:
                repo = os.path.join(td, "empty")  # no code files -> R4/R5 not judgeable
                os.makedirs(repo)
            cmd = [sys.executable, TOOL, pol, "--repo", repo]
            for name in externs:
                cmd += ["--extern", name]
            return subprocess.run(cmd, capture_output=True, text=True)

    def make_repo(self, td, resmgr_src=RESMGR_CPP):
        repo = os.path.join(td, "repo")
        os.makedirs(os.path.join(repo, "src", "platform", "qnx"))
        os.makedirs(os.path.join(repo, "apps"))
        with open(os.path.join(repo, "src", "platform", "qnx", "qnx_resmgr.cpp"),
                  "w", encoding="utf-8") as fh:
            fh.write(resmgr_src)
        with open(os.path.join(repo, "apps", "relmw_supervisor.cpp"),
                  "w", encoding="utf-8") as fh:
            fh.write(SUPERVISOR_CPP)
        return repo

    def assert_clean(self, proc):
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r"secpol_lint: OK \(\d+ statements, \d+ types\)")

    def assert_rule(self, proc, rule, needle=None):
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        hits = [l for l in proc.stdout.splitlines() if (": %s: " % rule) in l]
        self.assertTrue(hits, "no %s finding in:\n%s" % (rule, proc.stdout))
        if needle is not None:
            self.assertTrue(any(needle in l for l in hits),
                            "no %s finding mentioning %r in:\n%s" % (rule, needle, proc.stdout))

    def test_shipped_policy_is_clean(self):
        # the exact command ctest runs; the shipped policy must lint clean
        proc = subprocess.run([sys.executable, TOOL, POLICY, "--repo", ROOT],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("secpol_lint: OK (12 statements, 3 types)", proc.stdout)

    def test_usage_error_exit_2(self):
        proc = subprocess.run([sys.executable, TOOL], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)
        proc = subprocess.run([sys.executable, TOOL, "/no/such/policy.txt", "--repo", ROOT],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)

    def test_r1_undeclared_type(self):
        proc = self.run_lint(
            "type local_t;\n"
            "allow local_t self:ability { pathspace };\n"
            "allow ghost_t self:ability { pathspace };\n")
        self.assert_rule(proc, "R1", "ghost_t")

    def test_r2_unknown_ability(self):
        proc = self.run_lint(
            "type local_t;\nallow local_t self:ability { pathspace nosuchability };\n")
        self.assert_rule(proc, "R2", "nosuchability")

    def test_r3_forbidden_ability(self):
        proc = self.run_lint(
            "type local_t;\nallow local_t self:ability { unlock };\n")
        self.assert_rule(proc, "R3", "unlock")

    def test_r3_forbidden_attach_path(self):
        proc = self.run_lint(
            "type local_t;\nallow_attach local_t /dev/x*;\n")
        self.assert_rule(proc, "R3", "*")

    def test_r4_missing_coverage(self):
        # temp repo where a new resmgr_attach(mount_ + "/debug") is added
        with tempfile.TemporaryDirectory() as td:
            repo = self.make_repo(td, RESMGR_CPP_DEBUG)
            proc = self.run_lint(CLEAN_POLICY, repo)
            self.assert_rule(proc, "R4", "/dev/relmw/debug")

    def test_r5_overprivileged_path(self):
        with tempfile.TemporaryDirectory() as td:
            repo = self.make_repo(td)
            proc = self.run_lint(
                CLEAN_POLICY + "allow_attach relmw_supervisor_t /dev/relmw/extra;\n", repo)
            self.assert_rule(proc, "R5", "/dev/relmw/extra")

    def test_r6_unused_type(self):
        proc = self.run_lint(
            "type local_t;\ntype unused_t;\nallow local_t self:ability { pathspace };\n")
        self.assert_rule(proc, "R6", "unused_t")

    def test_missing_semicolon(self):
        proc = self.run_lint("type local_t\n")
        self.assert_rule(proc, "PARSE", "missing")

    def test_unbalanced_brace(self):
        proc = self.run_lint(
            "type local_t;\nallow local_t self:ability { pathspace;\n")
        self.assert_rule(proc, "PARSE", "unbalanced")

    def test_unknown_statement(self):
        proc = self.run_lint("frobnicate x;\n")
        self.assert_rule(proc, "PARSE", "unknown statement")

    def test_unknown_ability_single_name_form(self):
        proc = self.run_lint(
            "type local_t;\nallow local_t self:ability bogus_ability;\n")
        self.assert_rule(proc, "R2", "bogus_ability")

    def test_ability_names_from_80_reference(self):
        # reviewer regression: names that are NOT in the 8.0 Abilities Reference must be rejected, real ones accepted
        self.assert_clean(self.run_lint(
            "type local_t;\nallow local_t self:ability { high_resolution_timer schedule sandbox };\n"))
        for bogus in ("clockperiod", "timer", "keydata", "v86"):
            self.assert_rule(self.run_lint("type local_t;\nallow local_t self:ability %s;\n" % bogus), "R2", bogus)

    def test_ability_case_insensitive(self):
        proc = self.run_lint(
            "type local_t;\nallow local_t self:ability { PATHSPACE Public_Channel };\n")
        self.assert_clean(proc)

    def test_set_syntax(self):
        proc = self.run_lint(
            "type a_t;\ntype b_t;\ntype c_t;\nallow { a_t b_t } c_t:channel connect;\n")
        self.assert_clean(proc)

    def test_ftype_prefix_parsing(self):
        # ftype:/ftype+: prefixes are stripped before coverage matching
        with tempfile.TemporaryDirectory() as td:
            repo = self.make_repo(td)
            proc = self.run_lint(
                "type relmw_supervisor_t;\n"
                "allow_attach relmw_supervisor_t {\n"
                "    ftype:/dev/relmw/status\n"
                "    ftype+:/dev/relmw/events\n"
                "    /dev/relmw/ctl\n"
                "};\n"
                "allow relmw_supervisor_t self:ability { pathspace };\n",
                repo)
            self.assert_clean(proc)

    def test_extern_comment(self):
        proc = self.run_lint(
            "# extern: ghost_t\ntype local_t;\nallow local_t ghost_t:channel connect;\n")
        self.assert_clean(proc)

    def test_extern_flag(self):
        proc = self.run_lint(
            "type local_t;\nallow local_t ghost_t:channel connect;\n", externs=("ghost_t",))
        self.assert_clean(proc)

    def test_ranges(self):
        proc = self.run_lint(
            "# extern: ghost_t\ntype local_t;\n"
            "allow ghost_t self:ability { setuid:4-6,23 };\n"
            "allow local_t self:ability { priority:64-70 };\n")
        self.assert_clean(proc)

    def test_priority_without_range(self):
        proc = self.run_lint(
            "type local_t;\nallow local_t self:ability { priority };\n")
        self.assert_rule(proc, "R3", "priority")


if __name__ == "__main__":
    unittest.main()
