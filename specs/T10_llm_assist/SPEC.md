# T10: host-side LLM assistant (checklist F2) — task decomposition + failure explanation, OFF the real-time path

Working directory: repository root. Python 3 standard library ONLY (urllib, json, http.server for tests; no pip packages).
Read first: scripts/relmwctl, tools/make_dashboard.py, tools/test_make_dashboard.py (style), results/host_linux/episodes.jsonl
(one JSON object per episode: look at the fields, e.g. attempts with subtask/strategy/mode/correlated), CMakeLists.txt.
Do NOT touch C++ sources, src/platform/qnx/, tools/qnx_stubs/.

## Design rule (put it in the module docstring)
The LLM never runs inside the control loop. It runs on the development host, produces a small validated artefact (a plan line
or an explanation text), and the plan reaches the supervisor through its normal control surface
(`plan push_B,push_A` written to `/dev/relmw/ctl` on QNX, `<dir>/ctl` FIFO on the Linux host). Every LLM output is validated
against a closed vocabulary; anything invalid falls back to a deterministic rule-based result. With no endpoint configured the
tool still works (fallback only) and says so.

## Configuration (environment only; never hard-code URLs or keys; never print or log the key)
- base URL: `OPENAI_BASE_URL` if set, else `OPENAI_PUBLIC_BASE_URL`; OpenAI-compatible `POST {base}/chat/completions`
  (strip a trailing `/`; if base does not end with `/v1`, still just append `/chat/completions`).
- key: `OPENAI_API_KEY` (Authorization: Bearer). Model: `LLM_MODEL` (default "deepseek-v4.1-flash"). `--env-file FILE`
  optionally loads KEY=VALUE lines (ignore comments/blank) into the environment for this process only (files like this are
  git-ignored: `*.env`). Timeout 30 s, temperature 0, one retry on HTTP 429/5xx after 2 s.
- Errors (no base URL, network, non-JSON, HTTP error) -> fallback + a one-line warning on stderr WITHOUT the key, URL query or
  headers.

## New file `tools/llm_assist.py` (chmod +x, `#!/usr/bin/env python3`, SPDX MIT + "Copyright (c) 2026 elidot")
Subcommands:
1. `plan "<instruction>" [--apply DIR] [--relmwctl PATH] [--json]`
   - Vocabulary: subtasks `push_A`, `push_B` (cube A red -> goal A, cube B blue -> goal B). A plan is a non-empty ordered list,
     no duplicates, only vocabulary items.
   - LLM prompt asks for JSON only: `{"subtasks": [...], "reason": "..."}`. Parse the first JSON object in the reply (tolerate
     ```json fences). Validate. Invalid -> fallback.
   - Fallback parser: find mentions of A/red and B/blue in order of appearance in the instruction (word-boundary, case-
     insensitive: "A", "cube A", "red", "B", "blue"); "both"/"all"/no mention -> push_A,push_B; "only"/"just" + one object ->
     that one only.
   - Output: the control line `plan push_B,push_A` on stdout (plus with --json: `{"subtasks":..., "source":"llm"|"fallback",
     "reason":...}`). --apply DIR: run `<relmwctl> -m DIR plan push_B,push_A` (relmwctl default: scripts/relmwctl next to this
     repo), propagate its exit code. Reasons are not sent to the supervisor.
2. `explain (--episodes FILE.jsonl | --events FILE) [--max-episodes N] [--out FILE.md]`
   - First compute DETERMINISTIC facts in Python: per failure mode count, correlated count, strategies tried and how often they
     succeeded, escalations, per fault counts if present. (episodes.jsonl: use the real field names you find; events: lines
     like `ep=1003 tick=812 VerifyFail push_A attempt=1 strategy=... mode=off_axis_drift (correlated)` as printed by
     `relmwctl events`.)
   - Ask the LLM to explain these facts for an operator in <= 150 words, English, citing only numbers present in the facts.
   - Output markdown: a "Facts (computed)" table, then "Explanation (LLM: <model>)" or "Explanation (template, no LLM)".
   - Guard: if the LLM text contains a number (regex `\d+(\.\d+)?`) that does not appear in the facts, drop the LLM text and
     use the template, note "LLM output rejected: unsupported number".
3. `selftest` : runs plan+explain against built-in sample data with the configured endpoint and prints source per step
   (used for a manual live smoke test; not part of ctest).

## relmwctl
Add command `plan <subtask[,subtask...]>` to scripts/relmwctl (same reply handling as `fault`; usage text and header comment
updated). The supervisor side (`plan` handling in apps/relmw_supervisor.cpp) is done in another task; do not touch C++.

## Tests `tools/test_llm_assist.py` (unittest; register in CMakeLists.txt next to test_dashboard as `test_llm_assist`, inside
`if(Python3_FOUND)`). Use a local mock OpenAI server (http.server on 127.0.0.1, random port, in a thread) and set env vars in
the test only (base URL = mock, key = "test-key-not-real"). Cases:
- valid LLM plan reversed order -> `plan push_B,push_A`, source llm; request had Authorization header, model, temperature 0.
- LLM returns fenced JSON -> accepted; returns prose / unknown subtask / duplicates / empty list -> fallback.
- HTTP 500 twice -> fallback; 429 then 200 -> llm (retry); server not reachable -> fallback; no base URL at all -> fallback.
- fallback parser: "push the blue cube first, then the red one" -> push_B,push_A; "only cube A" -> push_A; "tidy up" ->
  push_A,push_B.
- the key never appears in stdout/stderr in any case (capture both).
- explain on results/host_linux/episodes.jsonl (first 40 episodes) -> facts table present, numbers match an independent count
  done in the test; LLM text with an unsupported number is rejected; text with only fact numbers accepted.
- --apply with a fake relmwctl script (temp file echoing its args) -> called with `-m DIR plan push_B,push_A`.
- `--env-file` loads variables; values with `=` inside survive.
Total runtime < 20 s.

## Docs
`docs/llm_assist.md` (short, English): what it does, the real-time boundary, configuration via env vars (names only), example
session on the Linux host and on QNX (`ssh root@<target> "echo 'plan push_B,push_A' > /dev/relmw/ctl"`), validation rules.

## Acceptance
```
python3 tools/test_llm_assist.py -v 2>&1 | tail -5
rm -rf /tmp/relmw_t10 && cmake -S . -B /tmp/relmw_t10 -G Ninja >/dev/null && cmake --build /tmp/relmw_t10 >/dev/null && ctest --test-dir /tmp/relmw_t10 -R "llm|dashboard" --output-on-failure | tail -3
sh -n scripts/relmwctl && (command -v shellcheck >/dev/null && shellcheck -s sh scripts/relmwctl || true)
grep -rnE "sk-[A-Za-z0-9]{8}|Bearer [A-Za-z0-9]{12}" tools docs scripts && echo LEAK || echo NO_KEYS
```
Do not call any real LLM endpoint during the task (tests use the mock only). No git commits. Budget: 20 minutes.

## Final reply
Files + one line each, test output tail, anything you could not do.
