#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
"""Tests for tools/llm_assist.py (registered in ctest as test_llm_assist).

All LLM traffic goes to a local mock OpenAI-compatible server
(http.server on 127.0.0.1, random port, in a thread). No real endpoint is
ever contacted; the key used is the literal string "test-key-not-real".
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "llm_assist.py")
EPISODES = os.path.join(ROOT, "results", "host_linux", "episodes.jsonl")
KEY = "test-key-not-real"

STATE = {"responses": [], "requests": []}


def chat_reply(content):
    return json.dumps({"choices": [{"message": {"content": content}}]})


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n) if n else b""
            state["requests"].append({
                "path": self.path,
                "auth": self.headers.get("Authorization"),
                "ctype": self.headers.get("Content-Type"),
                "body": body.decode("utf-8", "replace"),
            })
            if state["responses"]:
                status, payload = state["responses"].pop(0)
            else:
                status, payload = 200, chat_reply(json.dumps(
                    {"subtasks": ["push_B", "push_A"], "reason": "blue first"}))
            data = payload.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *args):
            pass

    return Handler


def independent_episode_counts(path, limit):
    """Recompute the deterministic facts the hard way, for cross-checking."""
    modes, tried, okd, faults = {}, {}, {}, {}
    corr = esc = eps = 0
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or eps >= limit:
                continue
            e = json.loads(line)
            eps += 1
            faults[e.get("fault", "?")] = faults.get(e.get("fault", "?"), 0) + 1
            if e.get("abort"):
                esc += 1
            for a in e.get("attempts", []):
                if a.get("mode") and a["mode"] != "none":
                    modes[a["mode"]] = modes.get(a["mode"], 0) + 1
                if a.get("correlated"):
                    corr += 1
                s = a.get("strategy")
                if s:
                    tried[s] = tried.get(s, 0) + 1
                    if a.get("ok"):
                        okd[s] = okd.get(s, 0) + 1
    return {"episodes": eps, "modes": modes, "correlated": corr, "escalations": esc,
            "tried": tried, "ok": okd, "faults": faults}


class MockServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = HTTPServer(("127.0.0.1", 0), make_handler(STATE))
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        STATE["responses"] = []
        STATE["requests"] = []

    # ---- helpers -------------------------------------------------------

    def run_tool(self, args, base=True, env_extra=None):
        env = {k: v for k, v in os.environ.items()
               if k not in ("OPENAI_BASE_URL", "OPENAI_PUBLIC_BASE_URL",
                            "OPENAI_API_KEY", "LLM_MODEL")}
        env["OPENAI_API_KEY"] = KEY
        if base:
            env["OPENAI_BASE_URL"] = "http://127.0.0.1:%d" % self.port
        if env_extra:
            env.update(env_extra)
        proc = subprocess.run([sys.executable, TOOL] + args, capture_output=True,
                              text=True, env=env, cwd=ROOT, timeout=60)
        # the key must never leak into tool output, whatever happened
        self.assertNotIn(KEY, proc.stdout, "key leaked into stdout")
        self.assertNotIn(KEY, proc.stderr, "key leaked into stderr")
        return proc

    def last_request(self):
        self.assertTrue(STATE["requests"], "mock server saw no request")
        return STATE["requests"][-1]

    # ---- plan ----------------------------------------------------------

    def test_plan_valid_llm_reversed_order(self):
        STATE["responses"] = [(200, chat_reply(json.dumps(
            {"subtasks": ["push_B", "push_A"], "reason": "blue first"})))]
        proc = self.run_tool(["plan", "push the blue cube first, then the red one",
                              "--json"], env_extra={"LLM_MODEL": "test-model-7"})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        lines = proc.stdout.strip().splitlines()
        self.assertEqual(lines[0], "plan push_B,push_A")
        obj = json.loads(lines[1])
        self.assertEqual(obj["subtasks"], ["push_B", "push_A"])
        self.assertEqual(obj["source"], "llm")
        # the request carried the auth header, the model and temperature 0
        req = self.last_request()
        self.assertEqual(req["auth"], "Bearer " + KEY)
        body = json.loads(req["body"])
        self.assertEqual(body["model"], "test-model-7")
        self.assertEqual(body["temperature"], 0)
        self.assertEqual(req["path"], "/chat/completions")

    def test_plan_base_url_v1_with_trailing_slash(self):
        STATE["responses"] = [(200, chat_reply(json.dumps(
            {"subtasks": ["push_A"], "reason": "only A"})))]
        proc = self.run_tool(["plan", "only cube A"],
                             env_extra={"OPENAI_BASE_URL":
                                        "http://127.0.0.1:%d/v1/" % self.port})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.last_request()["path"], "/v1/chat/completions")

    def test_plan_fenced_json_accepted(self):
        content = "```json\n" + json.dumps(
            {"subtasks": ["push_A", "push_B"], "reason": "A then B"}) + "\n```"
        STATE["responses"] = [(200, chat_reply(content))]
        proc = self.run_tool(["plan", "push both cubes", "--json"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("plan push_A,push_B", proc.stdout)
        self.assertEqual(json.loads(proc.stdout.splitlines()[1])["source"], "llm")

    def test_plan_invalid_llm_output_falls_back(self):
        bad_replies = [
            "Sure! I would push them one by one.",                       # prose
            json.dumps({"subtasks": ["push_C"]}),                        # unknown subtask
            json.dumps({"subtasks": ["push_A", "push_A"]}),              # duplicates
            json.dumps({"subtasks": []}),                                # empty list
        ]
        for reply in bad_replies:
            STATE["responses"] = [(200, chat_reply(reply))]
            STATE["requests"] = []
            proc = self.run_tool(["plan", "tidy up", "--json"])
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("plan push_A,push_B", proc.stdout)
            self.assertEqual(json.loads(proc.stdout.splitlines()[1])["source"], "fallback")
            self.assertTrue(proc.stderr.strip(), "expected a one-line stderr warning")

    def test_plan_http_500_twice_then_fallback(self):
        STATE["responses"] = [(500, "boom"), (500, "boom")]
        proc = self.run_tool(["plan", "tidy up", "--json"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("plan push_A,push_B", proc.stdout)
        self.assertEqual(json.loads(proc.stdout.splitlines()[1])["source"], "fallback")
        self.assertEqual(len(STATE["requests"]), 2, "expected exactly one retry")
        self.assertIn("HTTP 500", proc.stderr)

    def test_plan_429_then_200_uses_llm(self):
        STATE["responses"] = [(429, "slow down"),
                              (200, chat_reply(json.dumps(
                                  {"subtasks": ["push_B"], "reason": "blue only"})))]
        proc = self.run_tool(["plan", "only the blue cube", "--json"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("plan push_B", proc.stdout)
        self.assertEqual(json.loads(proc.stdout.splitlines()[1])["source"], "llm")
        self.assertEqual(len(STATE["requests"]), 2)

    def test_plan_server_unreachable(self):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        dead_port = sock.getsockname()[1]
        sock.close()
        proc = self.run_tool(["plan", "tidy up", "--json"],
                             env_extra={"OPENAI_BASE_URL":
                                        "http://127.0.0.1:%d" % dead_port})
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("plan push_A,push_B", proc.stdout)
        self.assertEqual(json.loads(proc.stdout.splitlines()[1])["source"], "fallback")
        self.assertTrue(proc.stderr.strip())

    def test_plan_no_base_url_at_all(self):
        proc = self.run_tool(["plan", "tidy up", "--json"], base=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("plan push_A,push_B", proc.stdout)
        self.assertEqual(json.loads(proc.stdout.splitlines()[1])["source"], "fallback")
        self.assertIn("fallback", proc.stderr.lower())

    def test_fallback_parser_cases(self):
        cases = [("push the blue cube first, then the red one", "plan push_B,push_A"),
                 ("only cube A", "plan push_A"),
                 ("tidy up", "plan push_A,push_B"),
                 # reviewer regression: the article "a" is not cube A
                 ("put the blue cube in a corner", "plan push_B"),
                 ("Push cube a to its goal", "plan push_A"),
                 ("B first, then A", "plan push_B,push_A")]
        for instruction, expected in cases:
            proc = self.run_tool(["plan", instruction], base=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(proc.stdout.strip(), expected)

    # ---- explain -------------------------------------------------------

    def test_explain_success_per_arm_and_filters(self):
        # reviewer addition: the three arms are paired runs of the same seeds; report success per arm/condition
        with open(EPISODES, encoding="utf-8") as efh:
            rows = [json.loads(l) for l in efh if l.strip()]
        field_mw = [r for r in rows if r["arm"] == "corr_aware_retry" and r["condition"] == "field"]
        ok = sum(1 for r in field_mw if r["gt_success"])
        proc = self.run_tool(["explain", "--episodes", EPISODES, "--arm", "corr_aware_retry", "--condition", "field"],
                             base=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("| episodes | %d |" % len(field_mw), proc.stdout)
        self.assertIn("| success corr_aware_retry/field | %d of %d |" % (ok, len(field_mw)), proc.stdout)
        self.assertNotIn("single_pass", proc.stdout)

    def test_explain_episodes_facts_and_number_guard(self):
        want = independent_episode_counts(EPISODES, 40)
        # accepted: LLM text cites only numbers that are in the facts
        STATE["responses"] = [(200, chat_reply(
            "The log covers %d episodes. Strategy first_try succeeded %d times out of %d."
            % (want["episodes"], want["ok"]["first_try"], want["tried"]["first_try"])))]
        proc = self.run_tool(["explain", "--episodes", EPISODES, "--max-episodes", "40"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("## Facts (computed)", proc.stdout)
        self.assertIn("| fact | value |", proc.stdout)
        self.assertIn("## Explanation (LLM:", proc.stdout)
        # facts table numbers match the independent count
        self.assertIn("| episodes | %d |" % want["episodes"], proc.stdout)
        self.assertIn("| escalations | %d |" % want["escalations"], proc.stdout)
        self.assertIn("| correlated attempts | %d |" % want["correlated"], proc.stdout)
        for mode, cnt in want["modes"].items():
            self.assertIn("| failure mode %s | %d |" % (mode, cnt), proc.stdout)
        for s in want["tried"]:
            self.assertIn("| strategy %s | %d tried, %d ok |"
                          % (s, want["tried"][s], want["ok"].get(s, 0)), proc.stdout)
        for f, cnt in want["faults"].items():
            self.assertIn("| fault %s | %d |" % (f, cnt), proc.stdout)

        # rejected: LLM text contains a number absent from the facts
        STATE["responses"] = [(200, chat_reply("There were 12345 failures here."))]
        proc = self.run_tool(["explain", "--episodes", EPISODES, "--max-episodes", "40"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("LLM output rejected: unsupported number", proc.stdout)
        self.assertIn("## Explanation (template, no LLM)", proc.stdout)
        self.assertNotIn("12345", proc.stdout)

        # no endpoint at all -> template
        proc = self.run_tool(["explain", "--episodes", EPISODES, "--max-episodes", "40"],
                             base=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("## Explanation (template, no LLM)", proc.stdout)
        self.assertIn("## Facts (computed)", proc.stdout)

    def test_explain_events_input_and_out_file(self):
        with tempfile.TemporaryDirectory() as td:
            ev = os.path.join(td, "events.txt")
            with open(ev, "w", encoding="utf-8") as fh:
                fh.write("\n".join([
                    "EPISODE 2001 start arm=mw fault=bump",
                    "ep=2001 tick=0 SUBTASK_START push_A attempt=0 strategy=first_try mode=none",
                    "ep=2001 tick=200 VERIFY_FAIL push_A attempt=0 strategy=first_try mode=off_axis_drift (correlated)",
                    "ep=2001 tick=217 RETRY push_A attempt=1 strategy=relocalize mode=none",
                    "ep=2001 tick=257 SUBTASK_OK push_A attempt=1 strategy=relocalize mode=none",
                    "ep=2001 tick=300 ESCALATE push_A attempt=3 strategy= mode=wrong_side_stall",
                    "EPISODE 2001 SUCCESS",
                ]) + "\n")
            STATE["responses"] = [(200, chat_reply(
                "1 episode, 1 escalation, 1 correlated failure, 1 verify failure."))]
            out_md = os.path.join(td, "report.md")
            proc = self.run_tool(["explain", "--events", ev, "--out", out_md])
            self.assertEqual(proc.returncode, 0, proc.stderr)
            with open(out_md, encoding="utf-8") as fh:
                md = fh.read()
            self.assertIn("## Facts (computed)", md)
            self.assertIn("| episodes | 1 |", md)
            self.assertIn("| failure mode off_axis_drift | 1 |", md)
            self.assertIn("| correlated attempts | 1 |", md)
            self.assertIn("| escalations | 1 |", md)
            self.assertIn("| strategy relocalize | 1 tried, 1 ok |", md)   # reviewer fix: VERIFY_FAIL/OK are not tries
            self.assertIn("| strategy first_try | 1 tried, 0 ok |", md)
            self.assertIn("| fault bump | 1 |", md)
            self.assertIn("## Explanation (LLM:", md)

    # ---- apply / env-file / selftest ------------------------------------

    def test_apply_calls_relmwctl(self):
        with tempfile.TemporaryDirectory() as td:
            fake = os.path.join(td, "fakectl")
            with open(fake, "w", encoding="utf-8") as fh:
                fh.write("#!/bin/sh\nprintf '%s\\n' \"$*\"\nexit \"${FAKE_RC:-0}\"\n")
            os.chmod(fake, 0o755)
            ctl_dir = os.path.join(td, "mount")
            os.makedirs(ctl_dir)
            env_extra = {"FAKE_RC": "0"}
            proc = self.run_tool(["plan", "tidy up", "--apply", ctl_dir,
                                  "--relmwctl", fake], base=False, env_extra=env_extra)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("-m %s plan push_A,push_B" % ctl_dir, proc.stdout)
            # exit code of relmwctl is propagated
            proc = self.run_tool(["plan", "tidy up", "--apply", ctl_dir,
                                  "--relmwctl", fake], base=False,
                                 env_extra={"FAKE_RC": "3"})
            self.assertEqual(proc.returncode, 3)

    def test_env_file_loads_values_with_equals(self):
        with tempfile.TemporaryDirectory() as td:
            envf = os.path.join(td, "run.env")
            with open(envf, "w", encoding="utf-8") as fh:
                fh.write("# comment line\n\nLLM_MODEL=env-model-1\nOPENAI_API_KEY=k=with=equals\n")
            STATE["responses"] = [(200, chat_reply(json.dumps(
                {"subtasks": ["push_A"], "reason": "x"})))]
            proc = self.run_tool(["--env-file", envf, "plan", "only cube A", "--json"])
            self.assertEqual(proc.returncode, 0, proc.stderr)
            req = self.last_request()
            self.assertEqual(req["auth"], "Bearer k=with=equals")
            self.assertEqual(json.loads(req["body"])["model"], "env-model-1")

    def test_selftest_runs_with_and_without_endpoint(self):
        proc = self.run_tool(["selftest"])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertRegex(proc.stdout, r"plan: source=(llm|fallback)")
        self.assertRegex(proc.stdout, r"explain: source=(llm|template)")
        proc = self.run_tool(["selftest"], base=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("plan: source=fallback", proc.stdout)
        self.assertIn("explain: source=template", proc.stdout)


if __name__ == "__main__":
    unittest.main()
