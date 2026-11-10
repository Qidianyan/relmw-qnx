# T7: one-page offline dashboard (checklist D4)

Working directory: repository root. Python 3 standard library ONLY (no pip packages). Do not modify C++ code.

## New file `tools/make_dashboard.py`
`python3 tools/make_dashboard.py --host results/host_linux [--qnx results/qnx_qemu_x86_64] --out docs/dashboard.html`
Produces ONE self-contained HTML file (inline CSS + inline SVG; NO JavaScript CDN, NO external fonts/images/URLs) with:
1. Header: "relmw-qnx — reliability supervisor results", generation date, and the label field from summary.json
   (it says SIMULATION ONLY — keep that visible, it is an honesty requirement).
2. Table per condition (nominal, field) × arm (single_pass = "no middleware", naive_retry, corr_aware_retry = "this middleware"):
   success/N, success_rate with 95% CI, episodes_with_detected_failure, recovered, escalated_to_operator, mean_sim_steps[0],
   and for naive/corr rows vs_single_pass diff + mcnemar_p. Values come from `summary.json` (`conditions.<cond>.<arm>`).
3. Inline SVG bar chart: field success rate per arm with CI whiskers.
4. Per-fault table for the field condition from `by_fault` of each arm.
5. Real-time section: for every `rt_report*.json` in the host dir (and the qnx dir if given) a row with platform, sched,
   period_ns, episodes, success, release_latency p50/p99/p999/max in µs and deadline_misses/count; plus an inline SVG
   horizontal bar chart of p50/p99/max per report (log scale is fine; label units).
6. If `--qnx` is missing or the directory has no JSON, show a clearly marked placeholder row with the text
   `【待 QNX 实测】 (pending first run on QNX 8.0 in QEMU)` — NEVER invent QNX numbers.
7. Footer: list of input files used (relative paths) and their sha256 (first 12 hex chars).
- Robust to missing optional keys (print "–"). Escape all strings with html.escape.

## New test `tools/test_make_dashboard.py` (unittest) registered in ctest
In CMakeLists.txt inside `if(NOT RELMW_QNX)`: `find_package(Python3 COMPONENTS Interpreter)` and, if found,
`add_test(NAME test_dashboard COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_make_dashboard.py)`.
Tests (run against the real `results/host_linux`, writing output to a temp dir):
- output exists, contains "78", "80", "98" in the field table context and "SIMULATION ONLY";
- contains the 【待 QNX 实测】 placeholder when no --qnx dir; with a fake qnx dir (temp, containing a copied rt_report json whose
  "platform" is set to "QNX") the placeholder is absent and "QNX" row present;
- no `<script src`, no `http://` or `https://` substrings, no `<link` tags (fully offline);
- the HTML parses with html.parser without exceptions and every opened <svg> is closed.

## Also
Generate `docs/dashboard.html` from the current results and keep it in the repo.

## Acceptance
```
python3 tools/test_make_dashboard.py -v 2>&1 | tail -3 && python3 tools/make_dashboard.py --host results/host_linux --out docs/dashboard.html && rm -rf /tmp/relmw_t7 && CC=gcc CXX=g++ cmake -S . -B /tmp/relmw_t7 -G Ninja >/dev/null && cmake --build /tmp/relmw_t7 >/dev/null && ctest --test-dir /tmp/relmw_t7 -R dashboard --output-on-failure | tail -2
```
Budget: 15 minutes. Write files into the repo first, then iterate.

## Final reply
Files created, test output, size of docs/dashboard.html.
