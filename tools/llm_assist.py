#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
"""Host-side LLM assistant for relmw: task decomposition + failure explanation.

Design rule: the LLM never runs inside the control loop. It runs on the
development host, produces a small validated artefact (a plan line or an
explanation text), and the plan reaches the supervisor through its normal
control surface (`plan push_B,push_A` written to /dev/relmw/ctl on QNX,
<dir>/ctl FIFO on the Linux host) via scripts/relmwctl. Every LLM output is
validated against a closed vocabulary; anything invalid falls back to a
deterministic rule-based result. With no endpoint configured the tool still
works (fallback only) and says so.

Subcommands:
    plan "<instruction>" [--apply DIR] [--relmwctl PATH] [--json]
        Decompose an operator instruction into subtasks. Prints the control
        line `plan push_B,push_A` on stdout; with --json it adds a second line
        {"subtasks": [...], "source": "llm"|"fallback", "reason": "..."}.
        With --apply DIR the line is sent via `<relmwctl> -m DIR plan ...`
        and relmwctl's exit code is propagated.
    explain (--episodes FILE.jsonl | --events FILE) [--max-episodes N] [--out FILE.md]
        Deterministic facts are computed in Python first, then the LLM is asked
        to explain them for an operator (<= 150 words, English, citing only
        numbers present in the facts). Output is markdown: a "Facts (computed)"
        table, then "Explanation (LLM: <model>)" or "Explanation (template,
        no LLM)". LLM text containing a number absent from the facts is
        rejected and replaced by the template, with the note
        "LLM output rejected: unsupported number".
    selftest
        Runs plan+explain against built-in sample data with the configured
        endpoint and prints the source per step (manual live smoke test;
        not part of ctest).

Configuration (environment only; no URL or key is ever hard-coded, printed or
logged):
    OPENAI_BASE_URL / OPENAI_PUBLIC_BASE_URL  base URL; POST {base}/chat/completions
    OPENAI_API_KEY                            sent as "Authorization: Bearer <key>"
    LLM_MODEL                                 default "deepseek-v4.1-flash"
    --env-file FILE                           load KEY=VALUE lines into the
                                              environment for this process only
                                              (such files are git-ignored: *.env)

Timeout 30 s, temperature 0, one retry on HTTP 429/5xx after 2 s.
Python 3 standard library only.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_RELMWCTL = os.path.join(REPO_ROOT, "scripts", "relmwctl")

SUBTASKS = ("push_A", "push_B")
DEFAULT_MODEL = "deepseek-v4.1-flash"
LLM_TIMEOUT = 30.0
RETRY_DELAY = 2.0
NUM_RE = re.compile(r"\d+(?:\.\d+)?")

PLAN_PROMPT = (
    "You are the host-side planning assistant for the relmw reliability middleware.\n"
    "The only allowed subtasks are:\n"
    "- push_A: push cube A (red) to goal A\n"
    "- push_B: push cube B (blue) to goal B\n"
    "Reply with ONE JSON object and nothing else, exactly like:\n"
    '{"subtasks": ["push_A", "push_B"], "reason": "why this order"}\n'
    "Rules: subtasks must be a non-empty ordered list, no duplicates, only the two "
    "subtasks above.\n\n"
    "Operator instruction:\n"
)

EXPLAIN_PROMPT = (
    "You are writing a short explanation of recorded middleware results for an operator.\n"
    "Below are facts computed deterministically from the raw data. Explain them in at most "
    "150 words, in English, citing only numbers that appear in the facts. Do not invent any "
    "number, do not speculate about causes that are not in the facts.\n\n"
    "Facts:\n{facts}\n"
)


def warn(msg):
    """One-line warning on stderr. Never contains the key, URL query or headers."""
    print("llm_assist: warning: " + " ".join(str(msg).split()), file=sys.stderr)


# ---------------------------------------------------------------- config

def base_url():
    u = os.environ.get("OPENAI_BASE_URL") or os.environ.get("OPENAI_PUBLIC_BASE_URL") or ""
    return u.rstrip("/")


def model_name():
    return os.environ.get("LLM_MODEL") or DEFAULT_MODEL


def load_env_file(path):
    """Load KEY=VALUE lines (comments/blanks ignored) into this process' env."""
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, val = line.split("=", 1)
            os.environ[key.strip()] = val.strip()


# ---------------------------------------------------------------- llm call

class LlmError(Exception):
    pass


def chat(messages):
    """POST {base}/chat/completions; return the raw reply body. Raises LlmError."""
    base = base_url()
    if not base:
        raise LlmError("no OPENAI_BASE_URL/OPENAI_PUBLIC_BASE_URL configured")
    url = base + "/chat/completions"
    payload = json.dumps({
        "model": model_name(),
        "temperature": 0,
        "messages": messages,
    }).encode("utf-8")
    last = LlmError("LLM request failed")
    for attempt in range(2):
        req = urllib.request.Request(url, data=payload, method="POST", headers={
            "Authorization": "Bearer " + os.environ.get("OPENAI_API_KEY", ""),
            "Content-Type": "application/json",
            "Accept": "application/json",
        })
        try:
            with urllib.request.urlopen(req, timeout=LLM_TIMEOUT) as resp:
                return resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            code = e.code
            try:
                e.read()
            except Exception:
                pass
            last = LlmError("HTTP %d" % code)
            if attempt == 0 and (code == 429 or 500 <= code < 600):
                time.sleep(RETRY_DELAY)
                continue
            break
        except urllib.error.URLError as e:
            last = LlmError("network error: %s" % getattr(e, "reason", e))
            break
    raise last


def extract_content(raw):
    try:
        data = json.loads(raw)
        content = data["choices"][0]["message"]["content"]
    except Exception:
        raise LlmError("non-JSON response")
    if not isinstance(content, str):
        raise LlmError("non-JSON response")
    return content


def first_json_object(text):
    """Parse the first JSON object in the reply (tolerates ```json fences)."""
    start = text.find("{")
    if start < 0:
        return None
    try:
        obj, _ = json.JSONDecoder().raw_decode(text[start:])
    except Exception:
        return None
    return obj if isinstance(obj, dict) else None


# ---------------------------------------------------------------- plan

def validate_plan(obj):
    """Closed vocabulary: non-empty ordered list, no duplicates, push_A/push_B only."""
    if not isinstance(obj, dict):
        return None
    subs = obj.get("subtasks")
    if not isinstance(subs, list) or not subs:
        return None
    for s in subs:
        if s not in SUBTASKS:
            return None
    if len(set(subs)) != len(subs):
        return None
    reason = obj.get("reason", "")
    if not isinstance(reason, str):
        reason = ""
    return subs, reason


# A bare single letter counts only in upper case ("A", "B"): in lower case "a" is the article ("push the blue cube
# in a corner" must not mention cube A). "cube a" / "red" / "blue" are case-insensitive.
OBJ_A = re.compile(r"(?i:cube\s+a\b|\bred\b)|\bA\b")
OBJ_B = re.compile(r"(?i:cube\s+b\b|\bblue\b)|\bB\b")


def fallback_subtasks(instruction):
    """Deterministic rule-based parser (used when the LLM is absent or invalid)."""
    text = instruction or ""
    low = text.lower()
    if re.search(r"\b(both|all)\b", low):
        return ["push_A", "push_B"]
    marks = [(m.start(), "A") for m in OBJ_A.finditer(text)]
    marks += [(m.start(), "B") for m in OBJ_B.finditer(text)]
    if not marks:
        return ["push_A", "push_B"]
    marks.sort()
    objs = []
    for _pos, o in marks:
        if o not in objs:
            objs.append(o)
    if re.search(r"\b(only|just)\b", low) and len(objs) == 1:
        objs = objs[:1]
    return ["push_" + o for o in objs]


def plan_subtasks(instruction):
    """Return (subtasks, source, reason, warning). source is 'llm' or 'fallback'."""
    cause = None
    try:
        content = extract_content(chat([{"role": "user", "content": PLAN_PROMPT + instruction}]))
        validated = validate_plan(first_json_object(content))
        if validated is not None:
            return validated[0], "llm", validated[1], None
        cause = "LLM output failed validation"
    except LlmError as e:
        cause = str(e)
    subs = fallback_subtasks(instruction)
    if cause:
        warn(cause + "; using deterministic fallback")
    return subs, "fallback", "deterministic fallback: " + (cause or "no endpoint"), None


def cmd_plan(args):
    subs, source, reason, _w = plan_subtasks(args.instruction)
    line = "plan " + ",".join(subs)
    print(line)
    if args.json:
        print(json.dumps({"subtasks": subs, "source": source, "reason": reason}))
    if args.apply:
        ctl = args.relmwctl or DEFAULT_RELMWCTL
        try:
            return subprocess.run([ctl, "-m", args.apply, "plan", ",".join(subs)]).returncode
        except OSError as exc:
            warn("cannot run %s: %s" % (ctl, exc))
            return 1
    return 0


# ---------------------------------------------------------------- explain

def facts_from_episodes(path, max_episodes, arm=None, condition=None):
    facts = {"episodes": 0, "escalations": 0, "correlated": 0,
             "modes": {}, "strategies": {}, "faults": {}, "arms": {}}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                e = json.loads(line)
            except Exception:
                continue
            if arm and e.get("arm") != arm:
                continue
            if condition and e.get("condition") != condition:
                continue
            if max_episodes and facts["episodes"] >= max_episodes:
                break
            facts["episodes"] += 1
            # Success per arm (ground truth): the arms are paired runs of the same seeds, so mixing them in one
            # failure count would hide the comparison the operator actually cares about.
            key = "%s/%s" % (e.get("arm", "?"), e.get("condition", "?"))
            n, ok = facts["arms"].get(key, (0, 0))
            facts["arms"][key] = (n + 1, ok + (1 if e.get("gt_success") else 0))
            if e.get("abort"):
                facts["escalations"] += 1
            faults = facts["faults"]
            faults[e.get("fault", "?")] = faults.get(e.get("fault", "?"), 0) + 1
            for a in e.get("attempts") or []:
                mode = a.get("mode")
                if mode and mode != "none":
                    facts["modes"][mode] = facts["modes"].get(mode, 0) + 1
                if a.get("correlated"):
                    facts["correlated"] += 1
                s = a.get("strategy")
                if s:
                    t, o = facts["strategies"].get(s, (0, 0))
                    facts["strategies"][s] = (t + 1, o + (1 if a.get("ok") else 0))
    return facts


EVENT_RE = re.compile(
    r"^ep=(\d+)\s+tick=(\d+)\s+(\S+)\s+(\S+)\s+attempt=(\d+)\s+strategy=(\S*)\s+mode=(\S+)(.*)$")


def facts_from_events(path):
    facts = {"episodes": 0, "escalations": 0, "correlated": 0,
             "modes": {}, "strategies": {}, "faults": {}, "arms": {}}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("EPISODE "):
                m = re.search(r"\bfault=(\S+)", line)
                if m:
                    f = m.group(1)
                    facts["faults"][f] = facts["faults"].get(f, 0) + 1
                    facts["episodes"] += 1
                continue
            m = EVENT_RE.match(line)
            if not m:
                continue
            kind, strategy, mode, rest = m.group(3).upper(), m.group(6), m.group(7), m.group(8)
            if "(correlated)" in rest:
                facts["correlated"] += 1
            if kind == "VERIFY_FAIL" and mode != "none":
                facts["modes"][mode] = facts["modes"].get(mode, 0) + 1
            # An attempt starts with SUBTASK_START (first try / proactive fix) or RETRY and ends with SUBTASK_OK or
            # VERIFY_FAIL, all carrying the same strategy name: count the start as "tried", SUBTASK_OK as "ok".
            if strategy and kind in ("SUBTASK_START", "RETRY"):
                t, o = facts["strategies"].get(strategy, (0, 0))
                facts["strategies"][strategy] = (t + 1, o)
            elif strategy and kind == "SUBTASK_OK":
                t, o = facts["strategies"].get(strategy, (0, 0))
                facts["strategies"][strategy] = (t, o + 1)
            if kind == "ESCALATE":
                facts["escalations"] += 1
    return facts


def facts_markdown(facts):
    rows = [("episodes", facts["episodes"]),
            ("escalations", facts["escalations"]),
            ("correlated attempts", facts["correlated"])]
    for key in sorted(facts.get("arms", {})):
        n, ok = facts["arms"][key]
        rows.append(("success " + key, "%d of %d" % (ok, n)))
    for mode in sorted(facts["modes"]):
        rows.append(("failure mode " + mode, facts["modes"][mode]))
    for s in sorted(facts["strategies"]):
        tried, ok = facts["strategies"][s]
        rows.append(("strategy " + s, "%d tried, %d ok" % (tried, ok)))
    for f in sorted(facts["faults"]):
        rows.append(("fault " + f, facts["faults"][f]))
    lines = ["| fact | value |", "|---|---|"]
    for k, v in rows:
        lines.append("| %s | %s |" % (k, v))
    return "\n".join(lines)


def guard_ok(text, facts_text):
    """True when every number in text also appears in the facts."""
    allowed = {float(x) for x in NUM_RE.findall(facts_text)}
    for tok in NUM_RE.findall(text):
        if float(tok) not in allowed:
            return False
    return True


def template_explanation(facts):
    parts = ["%d episodes, %d escalations, %d correlated attempts."
             % (facts["episodes"], facts["escalations"], facts["correlated"])]
    if facts["modes"]:
        mode, cnt = sorted(facts["modes"].items(), key=lambda kv: (-kv[1], kv[0]))[0]
        parts.append("Most frequent failure mode: %s (%d)." % (mode, cnt))
    if facts["strategies"]:
        name, (tried, ok) = sorted(facts["strategies"].items(),
                                   key=lambda kv: (-kv[1][1], kv[0]))[0]
        parts.append("Best strategy: %s (%d ok of %d tried)." % (name, ok, tried))
    faults = {f: c for f, c in facts["faults"].items() if f != "none"}
    if faults:
        name, cnt = sorted(faults.items(), key=lambda kv: (-kv[1], kv[0]))[0]
        parts.append("Most often injected fault: %s (%d episodes)." % (name, cnt))
    return " ".join(parts)


def explain_prompt(table):
    return EXPLAIN_PROMPT.format(facts=table)


def build_explanation(facts):
    """Return (table, explanation, header, note). Uses the LLM only if valid."""
    table = facts_markdown(facts)
    text, header, note = template_explanation(facts), "Explanation (template, no LLM)", ""
    try:
        content = extract_content(chat([{"role": "user", "content": explain_prompt(table)}])).strip()
        if content and guard_ok(content, table):
            text, header = content, "Explanation (LLM: %s)" % model_name()
        elif content:
            note = "LLM output rejected: unsupported number"
    except LlmError as e:
        warn(str(e))
    return table, text, header, note


def cmd_explain(args):
    if args.episodes:
        facts = facts_from_episodes(args.episodes, args.max_episodes or 0, args.arm, args.condition)
    else:
        facts = facts_from_events(args.events)
    table, text, header, note = build_explanation(facts)
    parts = ["# relmw failure explanation", "", "## Facts (computed)", "", table, "",
             "## " + header, "", text]
    if note:
        parts += ["", "> " + note]
    md = "\n".join(parts) + "\n"
    if args.out:
        out_dir = os.path.dirname(os.path.abspath(args.out))
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(md)
    else:
        sys.stdout.write(md)
    return 0


# ---------------------------------------------------------------- selftest

SAMPLE_EVENTS = "\n".join([
    "EPISODE 2001 start arm=mw fault=bump",
    "ep=2001 tick=0 SUBTASK_START push_A attempt=0 strategy=first_try mode=none",
    "ep=2001 tick=200 VERIFY_FAIL push_A attempt=0 strategy=first_try mode=off_axis_drift (correlated)",
    "ep=2001 tick=217 RETRY push_A attempt=1 strategy=relocalize mode=none",
    "ep=2001 tick=257 SUBTASK_OK push_A attempt=1 strategy=relocalize mode=none",
    "EPISODE 2001 SUCCESS",
    "EPISODE 2002 start arm=mw fault=none",
    "ep=2002 tick=0 SUBTASK_START push_B attempt=0 strategy=first_try mode=none",
    "ep=2002 tick=60 SUBTASK_OK push_B attempt=0 strategy=first_try mode=none",
    "EPISODE 2002 SUCCESS",
]) + "\n"


def cmd_selftest(args):
    instruction = "push the blue cube first, then the red one"
    subs, source, reason, w = plan_subtasks(instruction)
    if w:
        warn(w)
    print("plan: source=%s subtasks=%s" % (source, ",".join(subs)))
    with tempfile.NamedTemporaryFile("w", suffix=".events", delete=False) as fh:
        fh.write(SAMPLE_EVENTS)
        path = fh.name
    try:
        table, text, header, note = build_explanation(facts_from_events(path))
        src = "llm" if header.startswith("Explanation (LLM") else "template"
        print("explain: source=%s note=%s" % (src, note or "none"))
        print(table)
        print(text)
    finally:
        os.unlink(path)
    return 0


# ---------------------------------------------------------------- cli

def add_env_file(p):
    p.add_argument("--env-file", default=argparse.SUPPRESS,
                   help="load KEY=VALUE lines into the environment for this process only")


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="llm_assist",
        description="Host-side LLM assistant for relmw (off the real-time path).")
    add_env_file(ap)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("plan", help="decompose an instruction into subtasks")
    add_env_file(p)
    p.add_argument("instruction")
    p.add_argument("--apply", metavar="DIR", help="send the plan via relmwctl -m DIR")
    p.add_argument("--relmwctl", metavar="PATH", help="relmwctl path (default: scripts/relmwctl)")
    p.add_argument("--json", action="store_true", help="also print the JSON result line")
    p.set_defaults(func=cmd_plan)

    e = sub.add_parser("explain", help="explain recorded failures")
    add_env_file(e)
    g = e.add_mutually_exclusive_group(required=True)
    g.add_argument("--episodes", metavar="FILE.jsonl")
    g.add_argument("--events", metavar="FILE")
    e.add_argument("--max-episodes", type=int, default=0)
    e.add_argument("--arm", help="only this arm (single_pass | naive_retry | corr_aware_retry)")
    e.add_argument("--condition", help="only this condition (nominal | field)")
    e.add_argument("--out", metavar="FILE.md")
    e.set_defaults(func=cmd_explain)

    s = sub.add_parser("selftest", help="manual live smoke test on built-in sample data")
    add_env_file(s)
    s.set_defaults(func=cmd_selftest)

    args = ap.parse_args(argv)
    env_file = getattr(args, "env_file", None)
    if env_file:
        try:
            load_env_file(env_file)
        except OSError as exc:
            warn("cannot read --env-file %s: %s" % (env_file, exc))
            return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
