# llm_assist: host-side LLM assistant (off the real-time path)

`tools/llm_assist.py` is a development-host helper for the relmw reliability
middleware. It does two jobs:

* `plan "<instruction>"` — decomposes an operator instruction into an ordered
  list of subtasks and prints the control line `plan push_B,push_A`;
* `explain` — turns recorded failure data (`episodes.jsonl` or `relmwctl events`
  output) into an operator-facing markdown report.

## Real-time boundary

The LLM never runs inside the control loop. It runs on the development host,
produces a small validated artefact (a plan line or an explanation text), and
the plan reaches the supervisor through its normal control surface
(`plan push_B,push_A` written to `/dev/relmw/ctl` on QNX, `<dir>/ctl` FIFO on
the Linux host) via `scripts/relmwctl`. Every LLM output is validated against a
closed vocabulary; anything invalid falls back to a deterministic rule-based
result. With no endpoint configured the tool still works (fallback only) and
says so on stderr.

## Configuration (environment variables only)

* `OPENAI_BASE_URL` (or `OPENAI_PUBLIC_BASE_URL`) — base URL of an
  OpenAI-compatible endpoint; the tool POSTs to `{base}/chat/completions`.
* `OPENAI_API_KEY` — sent as `Authorization: Bearer <key>`; never printed or
  logged.
* `LLM_MODEL` — model name (default `deepseek-v4.1-flash`).
* `--env-file FILE` — load `KEY=VALUE` lines (comments/blanks ignored) into the
  environment for this process only; such files are git-ignored (`*.env`).

Timeout 30 s, temperature 0, one retry on HTTP 429/5xx after 2 s. Any error
(no base URL, network, non-JSON, HTTP error) falls back with a one-line stderr
warning that contains no key, URL query or header.

## Validation rules

* `plan`: the vocabulary is closed — `push_A` (cube A red -> goal A) and
  `push_B` (cube B blue -> goal B). A plan is a non-empty ordered list with no
  duplicates; the first JSON object in the reply is parsed (markdown fences
  tolerated). Invalid output falls back to the deterministic parser (mentions
  of A/red and B/blue in order of appearance; `both`/`all`/no mention ->
  `push_A,push_B`; `only`/`just` + one object -> that one only).
* `explain`: facts are computed deterministically in Python first; the LLM
  explanation must cite only numbers present in the facts. A number that is not
  in the facts rejects the LLM text; the template explanation is used and the
  report notes `LLM output rejected: unsupported number`.
* Reasons are never sent to the supervisor.

## Example session (Linux host)

```sh
export OPENAI_BASE_URL=... OPENAI_API_KEY=... LLM_MODEL=...
python3 tools/llm_assist.py plan "push the blue cube first, then the red one"
# -> plan push_B,push_A
python3 tools/llm_assist.py explain --episodes results/host_linux/episodes.jsonl \
    --max-episodes 40 --out report.md
python3 tools/llm_assist.py selftest   # manual live smoke test (not in ctest)
```

## Example session (QNX target)

The plan line reaches the supervisor through the normal control surface:

```sh
ssh root@<target> "echo 'plan push_B,push_A' > /dev/relmw/ctl"
```

or via the operator CLI: `relmwctl plan push_B,push_A`
(`scripts/relmwctl plan <push_A[,push_B...]>`, same reply handling as `fault`).
