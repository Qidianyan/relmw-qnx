#!/usr/bin/env python3
"""Build the one-page offline dashboard (docs/dashboard.html).

Usage:
    python3 tools/make_dashboard.py --host results/host_linux \
        [--qnx results/qnx_qemu_x86_64] --out docs/dashboard.html

The output is ONE self-contained HTML file: inline CSS + inline SVG only,
no JavaScript, no external fonts/images/URLs (fully offline). Every number
comes from the result JSON files under results/; missing optional keys are
printed as "–". QNX numbers are never invented: without --qnx (or with a
QNX directory that contains no JSON) the real-time section shows the
explicit pending placeholder instead of data.
"""

import argparse
import datetime
import glob
import hashlib
import html
import json
import math
import os

DASH = "–"  # printed when an optional key is missing

QNX_PENDING = "【待 QNX 实测】 (pending first run on QNX 8.0 in QEMU)"

ARMS = (
    ("single_pass", "single_pass (no middleware)"),
    ("naive_retry", "naive_retry"),
    ("corr_aware_retry", "corr_aware_retry (this middleware)"),
)

CSS = """
body{font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;
     margin:24px auto;max-width:1040px;padding:0 16px;color:#1c1c1c;background:#fff;line-height:1.45}
h1{font-size:20px;margin:0 0 4px}
h2{font-size:15px;margin:30px 0 6px;border-bottom:2px solid #4a7ebb;padding-bottom:4px}
h3{font-size:13px;margin:16px 0 6px;color:#333}
.meta{color:#555;font-size:12px;margin:2px 0}
.banner{border:2px solid #a33;background:#fdeaea;padding:8px 12px;font-weight:700;
        border-radius:6px;display:inline-block;margin:8px 0 4px}
table{border-collapse:collapse;margin:8px 0;font-size:12px}
th,td{border:1px solid #ccc;padding:4px 8px;text-align:right;white-space:nowrap}
th{background:#f0f4fa;text-align:left}
td:first-child,th:first-child{text-align:left}
.mono{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:11px;text-align:left}
.pending{color:#8a6d00;background:#fff8e1;font-weight:700;text-align:left}
.note{color:#555;font-size:12px}
svg{display:block;margin:8px 0;max-width:100%;border:1px solid #e3e3e3}
footer{margin-top:32px;font-size:11px;color:#555;border-top:1px solid #ddd;padding-top:8px}
caption{caption-side:top;text-align:left;font-size:11px;color:#555;padding-bottom:4px}
"""


def esc(value):
    return html.escape(str(value), quote=True)


def load_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def sha12(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()[:12]


def raw(d, key):
    """Raw dict value, or None when the key is absent/None (caller prints DASH)."""
    if isinstance(d, dict):
        v = d.get(key)
        return None if v is None else v
    return None


def fmt(x, nd=1):
    if not isinstance(x, (int, float)) or isinstance(x, bool):
        return DASH
    return f"{x:.{nd}f}"


def fmt_int(x):
    if isinstance(x, bool) or not isinstance(x, (int, float)):
        return DASH
    if isinstance(x, float) and not float(x).is_integer():
        return f"{x:g}"
    return str(int(x))


def pct(rate, nd=1):
    if not isinstance(rate, (int, float)) or isinstance(rate, bool):
        return DASH
    return f"{rate * 100:.{nd}f}%"


def ci_text(ci):
    if not isinstance(ci, (list, tuple)) or len(ci) < 2:
        return DASH
    return f"{ci[0] * 100:.1f}%–{ci[1] * 100:.1f}%"


def us_of(ns):
    if not isinstance(ns, (int, float)) or isinstance(ns, bool):
        return DASH
    return f"{ns / 1000.0:.1f}"


# ---------------------------------------------------------------- sections

def fmt_p(x):
    if isinstance(x, bool) or not isinstance(x, (int, float)):
        return DASH
    if float(x).is_integer():
        return str(int(x))
    return f"{x:.4g}"


def condition_section(summary):
    """One table per condition × arm with the headline metrics."""
    conds = summary.get("conditions") or {}
    parts = ['<h2>1. Success by condition × arm</h2>']
    for cond in ("nominal", "field"):
        if cond not in conds:
            continue
        rows = []
        for key, label in ARMS:
            a = conds[cond].get(key)
            if not isinstance(a, dict):
                continue
            vs = a.get("vs_single_pass") or {}
            diff = vs.get("diff")
            diff_txt = DASH if diff is None else f"{diff * 100:+.1f} pp"
            p_txt = fmt_p(vs.get("mcnemar_p"))
            steps = a.get("mean_sim_steps")
            mean0 = steps[0] if isinstance(steps, list) and len(steps) > 0 else None
            rate = a.get("success_rate")
            ci = a.get("success_ci95")
            rate_txt = pct(rate)
            if isinstance(ci, (list, tuple)) and len(ci) >= 2 and isinstance(rate, (int, float)):
                rate_txt += f" (95% CI {ci_text(ci)})"
            rows.append(
                "<tr>"
                f"<td>{esc(label)}</td>"
                f"<td>{fmt_int(raw(a, 'success'))}/{fmt_int(raw(a, 'N'))}</td>"
                f"<td>{esc(rate_txt)}</td>"
                f"<td>{fmt_int(raw(a, 'episodes_with_detected_failure'))}</td>"
                f"<td>{fmt_int(raw(a, 'recovered'))}</td>"
                f"<td>{fmt_int(raw(a, 'escalated_to_operator'))}</td>"
                f"<td>{fmt(mean0)}</td>"
                f"<td>{esc(diff_txt)}</td>"
                f"<td>{esc(p_txt)}</td>"
                "</tr>"
            )
        parts.append(f"<h3>condition: {esc(cond)}</h3>")
        parts.append(
            "<table><caption>success/N, success rate with 95% CI, detected failures, "
            "recoveries, escalations, mean sim steps; naive/corr rows show the diff vs "
            "single_pass (pp) and McNemar p.</caption>"
            "<tr><th>arm</th><th>success/N</th><th>success rate (95% CI)</th>"
            "<th>episodes w/ detected failure</th><th>recovered</th>"
            "<th>escalated to operator</th><th>mean sim steps [0]</th>"
            "<th>vs single_pass diff</th><th>McNemar p</th></tr>"
            + "".join(rows)
            + "</table>"
        )
    return "\n".join(parts)


def field_arm_stats(summary):
    """(label, rate, ci_lo, ci_hi) per arm for the field condition."""
    out = []
    field = (summary.get("conditions") or {}).get("field") or {}
    for key, label in ARMS:
        a = field.get(key)
        if not isinstance(a, dict):
            continue
        rate = a.get("success_rate")
        if not isinstance(rate, (int, float)) or isinstance(rate, bool):
            continue
        ci = a.get("success_ci95")
        lo = ci[0] if isinstance(ci, (list, tuple)) and len(ci) >= 2 else rate
        hi = ci[1] if isinstance(ci, (list, tuple)) and len(ci) >= 2 else rate
        out.append((label, float(rate), float(lo), float(hi)))
    return out


def field_chart_svg(stats):
    """Horizontal bars: field success rate per arm, whiskers = 95% CI."""
    if not stats:
        return f"<p>{DASH}</p>"
    W, left, right = 760, 250, 660
    top, rowh = 36, 36
    H = top + rowh * len(stats) + 30

    def X(v):
        v = min(max(v, 0.0), 1.0)
        return left + (right - left) * v

    p = [f'<svg width="{W}" height="{H}" role="img" '
         f'aria-label="field condition success rate per arm with 95 percent CI whiskers">']
    for t in (0.0, 0.25, 0.5, 0.75, 1.0):
        x = X(t)
        p.append(f'<line x1="{x:.1f}" y1="{top - 8}" x2="{x:.1f}" y2="{H - 26}" stroke="#e0e0e0"/>')
        p.append(f'<text x="{x:.1f}" y="{H - 10}" font-size="10" text-anchor="middle" '
                 f'fill="#555">{t * 100:.0f}%</text>')
    for i, (label, rate, lo, hi) in enumerate(stats):
        y = top + i * rowh
        p.append(f'<text x="{left - 8}" y="{y + 13}" font-size="11" text-anchor="end" '
                 f'fill="#222">{esc(label)}</text>')
        p.append(f'<rect x="{left:.1f}" y="{y}" width="{max(X(rate) - left, 1.0):.1f}" '
                 f'height="18" fill="#4a7ebb"/>')
        xlo, xhi = X(lo), X(hi)
        p.append(f'<line x1="{xlo:.1f}" y1="{y + 9}" x2="{xhi:.1f}" y2="{y + 9}" '
                 f'stroke="#222" stroke-width="1.5"/>')
        p.append(f'<line x1="{xlo:.1f}" y1="{y + 3}" x2="{xlo:.1f}" y2="{y + 15}" '
                 f'stroke="#222" stroke-width="1.5"/>')
        p.append(f'<line x1="{xhi:.1f}" y1="{y + 3}" x2="{xhi:.1f}" y2="{y + 15}" '
                 f'stroke="#222" stroke-width="1.5"/>')
        p.append(f'<text x="{X(rate) + 6:.1f}" y="{y + 13}" font-size="11" fill="#222">'
                 f'{pct(rate)}</text>')
    p.append(f'<text x="{left:.1f}" y="16" font-size="11" fill="#555">'
             f'field condition · success rate (whiskers: 95% CI)</text>')
    p.append("</svg>")
    return "\n".join(p)


def fault_section(summary):
    """Per-fault table for the field condition, from each arm's by_fault."""
    field = (summary.get("conditions") or {}).get("field") or {}
    per_arm = {}
    faults = set()
    for key, _label in ARMS:
        bf = raw(field.get(key) or {}, "by_fault")
        if isinstance(bf, dict):
            per_arm[key] = bf
            faults.update(bf.keys())
    parts = ['<h2>3. Field condition: per-fault results</h2>']
    if not per_arm or not faults:
        parts.append(f"<p>{DASH}</p>")
        return "\n".join(parts)
    head = "<tr><th>fault</th>" + "".join(
        f"<th>{esc(label)}</th>" for _key, label in ARMS) + "</tr>"
    rows = []
    for fault in sorted(faults):
        cells = []
        for key, _label in ARMS:
            entry = (per_arm.get(key) or {}).get(fault)
            if isinstance(entry, dict) and "N" in entry and "success" in entry:
                n, s = entry["N"], entry["success"]
                rate = (s / n) if isinstance(n, (int, float)) and n else None
                cells.append(f"{fmt_int(s)}/{fmt_int(n)} ({pct(rate)})")
            else:
                cells.append(DASH)
        rows.append(f"<tr><td>{esc(fault)}</td>" +
                    "".join(f"<td>{esc(c)}</td>" for c in cells) + "</tr>")
    parts.append(
        "<table><caption>success/N (rate) per injected fault, field condition, "
        "from by_fault of each arm.</caption>" + head + "".join(rows) + "</table>")
    return "\n".join(parts)


def rt_rows(reports):
    """One table row per rt_report*.json."""
    rows = []
    for path, rep in reports:
        rl = rep.get("release_latency") if isinstance(rep, dict) else None
        rl = rl if isinstance(rl, dict) else {}
        dm, cnt = rl.get("deadline_misses"), rl.get("count")
        miss_txt = (f"{fmt_int(dm)}/{fmt_int(cnt)}"
                    if isinstance(dm, (int, float)) and isinstance(cnt, (int, float)) else DASH)
        rows.append(
            "<tr>"
            f"<td class='mono'>{esc(os.path.basename(path))}</td>"
            f"<td>{esc(raw(rep, 'platform'))}</td>"
            f"<td>{esc(raw(rep, 'sched'))}</td>"
            f"<td>{fmt_int(raw(rep, 'period_ns'))}</td>"
            f"<td>{fmt_int(raw(rep, 'episodes'))}</td>"
            f"<td>{fmt_int(raw(rep, 'success'))}</td>"
            f"<td>{us_of(rl.get('p50_ns'))}</td>"
            f"<td>{us_of(rl.get('p99_ns'))}</td>"
            f"<td>{us_of(rl.get('p999_ns'))}</td>"
            f"<td>{us_of(rl.get('max_ns'))}</td>"
            f"<td>{esc(miss_txt)}</td>"
            "</tr>")
    return "".join(rows)


RT_HEAD = ("<tr><th>report file</th><th>platform</th><th>sched</th><th>period_ns</th>"
           "<th>episodes</th><th>success</th><th>p50 µs</th><th>p99 µs</th><th>p999 µs</th>"
           "<th>max µs</th><th>deadline misses/count</th></tr>")

RT_COLORS = {"p50": "#4a7ebb", "p99": "#e0a030", "max": "#c0504d"}


def rt_chart_svg(reports):
    """Horizontal bars of p50/p99/max release latency per report, log scale (µs)."""
    groups, allv, dls = [], [], []
    for path, rep in reports:
        rl = rep.get("release_latency") if isinstance(rep, dict) else None
        rl = rl if isinstance(rl, dict) else {}
        vals = []
        for name, key in (("p50", "p50_ns"), ("p99", "p99_ns"), ("max", "max_ns")):
            v = rl.get(key)
            if isinstance(v, (int, float)) and not isinstance(v, bool) and v > 0:
                vals.append((name, v / 1000.0))  # ns -> µs for the chart
            else:
                vals.append((name, None))
        if all(v is None for _n, v in vals):
            continue
        groups.append((os.path.basename(path), rep, vals))
        allv.extend(v for _n, v in vals if v is not None)
        dl = rl.get("deadline_ns")
        if isinstance(dl, (int, float)) and not isinstance(dl, bool) and dl > 0:
            dls.append(dl / 1000.0)  # µs
    if not groups:
        return f"<p>{DASH}</p>"
    allv = allv + dls
    lo = math.floor(math.log10(min(allv)))
    hi = math.ceil(math.log10(max(allv)))
    if hi <= lo:
        hi = lo + 1

    W, left, right = 760, 230, 660
    top, rowh, ghdr, ggap = 40, 15, 18, 16
    H = top + len(groups) * (ghdr + 3 * rowh + ggap) + 26

    def X(v):
        return left + (right - left) * (math.log10(max(v, 1e-9)) - lo) / (hi - lo)

    p = [f'<svg width="{W}" height="{H}" role="img" '
         f'aria-label="release latency p50 p99 max per report, log scale, microseconds">']
    p.append(f'<text x="{left}" y="16" font-size="11" fill="#555">'
             f'release latency per report (µs, log scale)</text>')
    lx = 30
    for name in ("p50", "p99", "max"):
        p.append(f'<rect x="{lx}" y="8" width="9" height="9" fill="{RT_COLORS[name]}"/>')
        p.append(f'<text x="{lx + 13}" y="16" font-size="10" fill="#333">{name}</text>')
        lx += 52
    for k in range(lo, hi + 1):
        x = X(10 ** k)
        p.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{H - 22}" stroke="#eee"/>')
        p.append(f'<text x="{x:.1f}" y="{H - 8}" font-size="10" text-anchor="middle" '
                 f'fill="#555">{10 ** k:g} µs</text>')
    if dls:
        x = X(dls[0])
        p.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{H - 22}" '
                 f'stroke="#a33" stroke-dasharray="4 3"/>')
        p.append(f'<text x="{x + 3:.1f}" y="{top + 2}" font-size="9" fill="#a33">'
                 f'deadline {dls[0]:g} µs</text>')
    for i, (fname, rep, vals) in enumerate(groups):
        y0 = top + i * (ghdr + 3 * rowh + ggap)
        p.append(f'<text x="4" y="{y0 + 12}" font-size="11" fill="#222">'
                 f'{esc(fname)} · {esc(raw(rep, "platform"))} · sched={esc(raw(rep, "sched"))} '
                 f'· period {fmt_int(raw(rep, "period_ns"))} ns</text>')
        for j, (name, v) in enumerate(vals):
            y = y0 + ghdr + j * rowh
            p.append(f'<text x="{left - 8}" y="{y + 10}" font-size="10" text-anchor="end" '
                     f'fill="#555">{name}</text>')
            if v is None:
                continue
            p.append(f'<rect x="{left}" y="{y}" width="{max(X(v) - left, 1.0):.1f}" height="10" '
                     f'fill="{RT_COLORS[name]}"/>')
            p.append(f'<text x="{X(v) + 4:.1f}" y="{y + 9}" font-size="10" fill="#222">'
                     f'{v:.1f} µs</text>')
    p.append("</svg>")
    return "\n".join(p)


def rt_section(host_dir, host_reports, qnx_dir, qnx_reports, qnx_has_json):
    parts = ['<h2>4. Real-time scheduling reports (rt_report*.json)</h2>']
    parts.append(f"<h3>host: {esc(os.path.relpath(host_dir))}</h3>")
    if host_reports:
        parts.append("<table>" + RT_HEAD + rt_rows(host_reports) + "</table>")
    else:
        parts.append(f"<p class='note'>{DASH} no rt_report*.json in the host directory</p>")
    parts.append(f"<h3>QNX 8.0 (QEMU): {esc(qnx_dir) if qnx_dir else 'not provided'}</h3>")
    if qnx_dir is None or not qnx_has_json:
        parts.append(
            "<table><tr class='pending'><td colspan='11'>" + esc(QNX_PENDING) + "</td></tr></table>")
    elif qnx_reports:
        parts.append("<table>" + RT_HEAD + rt_rows(qnx_reports) + "</table>")
    else:
        parts.append(f"<p class='note'>{DASH} no rt_report*.json found in the QNX directory</p>")
    parts.append("<h3>release latency: p50 / p99 / max per report (µs, log scale)</h3>")
    parts.append(rt_chart_svg(host_reports + qnx_reports))
    return "\n".join(parts)


def build(host_dir, qnx_dir):
    summary_path = os.path.join(host_dir, "summary.json")
    summary = load_json(summary_path)
    inputs = [summary_path]

    host_reports = []
    for path in sorted(glob.glob(os.path.join(host_dir, "rt_report*.json"))):
        host_reports.append((path, load_json(path)))
        inputs.append(path)

    qnx_reports, qnx_has_json = [], False
    if qnx_dir:
        qnx_json = sorted(glob.glob(os.path.join(qnx_dir, "*.json")))
        qnx_has_json = bool(qnx_json)
        for path in qnx_json:
            if os.path.basename(path).startswith("rt_report"):
                qnx_reports.append((path, load_json(path)))
                inputs.append(path)

    label = raw(summary, "label")
    now = datetime.datetime.now().astimezone().isoformat(timespec="seconds")

    stats = field_arm_stats(summary)

    body = []
    body.append("<!DOCTYPE html>")
    body.append("<html lang=\"en\"><head><meta charset=\"utf-8\">")
    body.append("<title>relmw-qnx — reliability supervisor results</title>")
    body.append(f"<style>{CSS}</style></head><body>")
    body.append("<header>")
    body.append("<h1>relmw-qnx — reliability supervisor results</h1>")
    body.append(f"<p class='meta'>generated {esc(now)} · host results: {esc(os.path.relpath(host_dir))}"
                f" · N per arm: {esc(fmt_int(raw(summary, 'N_per_arm')))}"
                f" · library entries: {esc(fmt_int(raw(summary, 'library_entries')))}</p>")
    body.append(f"<p class='banner'>{esc(label)}</p>")
    body.append("</header>")
    body.append(condition_section(summary))
    body.append("<h2>2. Field condition: success rate per arm</h2>")
    body.append(field_chart_svg(stats))
    body.append(fault_section(summary))
    body.append(rt_section(host_dir, host_reports, qnx_dir, qnx_reports, qnx_has_json))
    body.append("<footer>")
    body.append("<h2>Input files used (relative paths, sha256 first 12 hex chars)</h2>")
    body.append("<table class='files'><tr><th>file</th><th>sha256[:12]</th></tr>")
    for path in inputs:
        body.append(f"<tr><td class='mono'>{esc(os.path.relpath(path))}</td>"
                    f"<td class='mono'>{esc(sha12(path))}</td></tr>")
    body.append("</table>")
    body.append("<p class='note'>generated by tools/make_dashboard.py (python3 standard library "
                "only); self-contained HTML: inline CSS + inline SVG, no scripts, no external "
                "resources.</p>")
    body.append("</footer></body></html>")
    return "\n".join(body) + "\n"


def main():
    ap = argparse.ArgumentParser(description="Build the offline one-page results dashboard.")
    ap.add_argument("--host", required=True, help="host results directory (summary.json + rt_report*.json)")
    ap.add_argument("--qnx", help="optional QNX results directory (rt_report*.json only)")
    ap.add_argument("--out", required=True, help="output HTML file")
    args = ap.parse_args()

    html_text = build(args.host, args.qnx)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(html_text)
    print(f"wrote {args.out} ({len(html_text)} bytes)")


if __name__ == "__main__":
    main()
