#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
"""Build the one-page results dashboard (docs/dashboard.html).

Usage:
    python3 tools/make_dashboard.py --host results/host_linux \
        [--qnx results/qnx_qemu_x86_64] [--sim DIR] --out docs/dashboard.html

The output is ONE self-contained HTML file: inline CSS, inline SVG (Phosphor icons vendored under
tools/dashboard_assets/phosphor, MIT), inline JS and the data embedded as JSON. No external fonts, images, scripts
or URLs; it works from file:// and when served statically. Every number comes from the result JSON files; missing
optional keys print as "–". QNX numbers are never invented: without --qnx (or with a QNX directory without JSON) the
real-time section shows the explicit pending placeholder. --sim adds rt_report*.json from the QNX API simulation
build in a separate block labelled "QNX API simulation on Linux, not QNX"; it is never merged with QNX numbers.

Function paths (JS): view state in the URL (?cond=nominal|field&arms=a,b&sec=id), Copy link / Share, download a
Markdown report or the JSON data, export a 1200x630 PNG summary card, print.
"""

import argparse
import datetime
import glob
import hashlib
import html
import json
import math
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(ROOT, "tools", "dashboard_assets", "phosphor", "regular")

DASH = "–"  # printed when an optional key is missing
QNX_PENDING = "【待 QNX 实测】 (pending first run on QNX 8.0 in QEMU)"
SIM_LABEL = "QNX API simulation on Linux, not QNX"
TITLE = "relmw-qnx — reliability supervisor results"

ARMS = (
    ("single_pass", "No middleware"),
    ("naive_retry", "Naive retry"),
    ("corr_aware_retry", "relmw (correlation-aware)"),
)
ARM_KEYS = [k for k, _ in ARMS]
ARM_LABEL = dict(ARMS)
ARM_COLOR = {"single_pass": "#8A8F98", "naive_retry": "#4A4F59", "corr_aware_retry": "#C2410C"}
CONDS = (("field", "Field faults"), ("nominal", "Nominal"))

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



def fmt_p(x):
    if isinstance(x, bool) or not isinstance(x, (int, float)):
        return DASH
    if float(x).is_integer():
        return str(int(x))
    return f"{x:.2g}" if x < 0.001 else f"{x:.3g}"


def num(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool)


# ---------------------------------------------------------------- icons

_ICON_CACHE = {}


def icon(name, cls="ph"):
    """Inline Phosphor regular icon from the vendored SVG (decorative: aria-hidden)."""
    if name not in _ICON_CACHE:
        with open(os.path.join(ICON_DIR, name + ".svg"), encoding="utf-8") as fh:
            svg = fh.read()
        inner = svg[svg.index(">") + 1: svg.rindex("</svg>")]
        _ICON_CACHE[name] = inner
    return (f'<svg class="{cls}" viewBox="0 0 256 256" fill="currentColor" aria-hidden="true" '
            f'focusable="false">{_ICON_CACHE[name]}</svg>')


# ---------------------------------------------------------------- styles

CSS = """
:root{--bg:#F6F4EF;--surface:#FFFFFF;--ink:#16181D;--ink-2:#4A4F59;--line:#E3E0D8;--accent:#C2410C;
--ok:#2F7D4F;--bad:#B42318;--pending:#9A6700;--halo:#FFFFFF;--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
*{box-sizing:border-box}
html{scroll-behavior:smooth;scroll-padding-top:120px}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif}
.ph{width:1.25em;height:1.25em;flex:none;vertical-align:-0.25em}
.skip{position:absolute;left:-9999px;top:8px;background:var(--surface);padding:8px 12px;border:1px solid var(--line);z-index:10}
.skip:focus{left:12px}
:focus-visible{outline:2px solid var(--accent);outline-offset:2px}
.top{position:sticky;top:0;z-index:5;background:rgba(246,244,239,.97);border-bottom:1px solid var(--line)}
.top-in{max-width:1320px;margin:0 auto;padding:10px 24px;display:flex;flex-wrap:wrap;align-items:center;gap:10px 16px}
.brand{display:flex;align-items:center;gap:10px;font-weight:600;white-space:nowrap}
.brand .mark{color:var(--accent)}
.brand .dim{color:var(--ink-2);font-weight:500}
.pill{display:inline-flex;align-items:center;gap:6px;border:1px solid #E7D3B0;background:#FBF3E4;color:var(--pending);
 font:600 11px/1 system-ui,sans-serif;letter-spacing:.06em;text-transform:uppercase;padding:6px 9px;border-radius:999px}
.actions{margin-left:auto;display:flex;flex-wrap:wrap;gap:6px}
.btn{display:inline-flex;align-items:center;gap:6px;font:500 13px/1 system-ui,sans-serif;color:var(--ink);background:var(--surface);
 border:1px solid var(--line);border-radius:8px;padding:8px 11px;cursor:pointer;min-height:36px}
.btn:hover{border-color:#CFCAC0}
.btn.primary{background:var(--accent);border-color:var(--accent);color:#fff}
.btn.primary:hover{background:#A9380A}
.status{font-size:12px;color:var(--ok);margin:0}.status:empty{display:none}
.wrap{max-width:1320px;margin:0 auto;padding:20px 24px 48px;display:grid;grid-template-columns:200px minmax(0,1fr);gap:32px}
nav.index{position:sticky;top:96px;align-self:start;font-size:13px}
nav.index ol{list-style:none;margin:0;padding:0;border-left:1px solid var(--line)}
nav.index a{display:block;padding:6px 12px;color:var(--ink-2);text-decoration:none;margin-left:-1px;border-left:2px solid transparent}
nav.index a:hover{color:var(--ink);border-left-color:var(--accent)}
nav.index .n{font-family:var(--mono);color:#8A8F98;margin-right:6px}
main{min-width:0}
h1{font-size:22px;font-weight:600;margin:0 0 4px;letter-spacing:-.01em}
.lede{color:var(--ink-2);margin:0 0 4px;max-width:78ch}
.meta{color:var(--ink-2);font-size:12px;margin:0}
.meta code,.mono{font-family:var(--mono);font-size:12px}
.controls{display:flex;flex-wrap:wrap;align-items:center;gap:12px 20px;margin:18px 0 16px;padding:12px 14px;background:var(--surface);
 border:1px solid var(--line);border-radius:10px}
.controls .lab{font:600 11px/1 system-ui,sans-serif;letter-spacing:.06em;text-transform:uppercase;color:var(--ink-2)}
.seg{display:inline-flex;border:1px solid var(--line);border-radius:8px;overflow:hidden}
.seg button{font:500 13px/1 system-ui,sans-serif;border:0;background:var(--surface);color:var(--ink-2);padding:8px 12px;cursor:pointer;min-height:34px}
.seg button+button{border-left:1px solid var(--line)}
.seg button[aria-pressed=true]{background:var(--ink);color:#fff}
.chk{display:inline-flex;align-items:center;gap:6px;font-size:13px;cursor:pointer}
.chk input{width:16px;height:16px;accent-color:var(--accent)}
.sw{display:inline-block;width:10px;height:10px;border-radius:2px}
.kpis{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px;margin:0 0 8px}
.kpi{background:var(--surface);border:1px solid var(--line);border-radius:10px;padding:14px 16px}
.kpi .k{display:flex;align-items:center;gap:6px;font:600 11px/1.2 system-ui,sans-serif;letter-spacing:.06em;text-transform:uppercase;color:var(--ink-2)}
.kpi .v{font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif;font-variant-numeric:tabular-nums lining-nums;font-size:32px;font-weight:650;margin:10px 0 4px;letter-spacing:-.025em;line-height:1.1}
.kpi .v.acc{color:var(--accent)}
.kpi .s{font-size:12px;color:var(--ink-2);display:grid;gap:2px}
.kpi .ln{display:block}
.kpi .ln.nw{white-space:nowrap;overflow:hidden;text-overflow:clip}
.kpi .s b{font-variant-numeric:tabular-nums;font-weight:600;color:var(--ink)}
.hero{display:grid;grid-template-columns:minmax(0,1fr);gap:6px;padding:8px 0 14px}
.eyebrow{display:flex;align-items:center;gap:8px;margin:0;font:600 12px/1.2 system-ui,sans-serif;letter-spacing:.06em;text-transform:uppercase;color:var(--ink-2)}
.hero h1{font-size:40px;line-height:1.1;font-weight:700;letter-spacing:-.03em;margin:6px 0 4px;max-width:30ch;font-variant-numeric:tabular-nums}
.hero h1 em{font-style:normal;color:var(--accent)}
.hero .sub{font-size:16px;color:var(--ink-2);margin:0;max-width:62ch}
.hero .sub b{color:var(--ink);font-variant-numeric:tabular-nums}
nav.index a[aria-current=true]{color:var(--ink);font-weight:600;border-left-color:var(--accent)}
nav.index a[aria-current=true] .n{color:var(--accent)}
section{margin-top:28px}
h2{display:flex;align-items:center;gap:8px;font:600 14px/1.2 system-ui,sans-serif;letter-spacing:.06em;text-transform:uppercase;margin:0 0 10px;color:var(--ink)}
h2 .n{font-family:var(--mono);color:var(--accent)}
h3{font-size:14px;font-weight:600;margin:16px 0 6px}
.card{background:var(--surface);border:1px solid var(--line);border-radius:10px;padding:16px}
.grid2{display:grid;grid-template-columns:minmax(0,1.1fr) minmax(0,1fr);gap:16px}
.stack{display:grid;grid-template-columns:minmax(0,1fr);gap:16px}
.bars{max-width:900px}
.bars-h{display:grid;grid-template-columns:minmax(120px,190px) minmax(0,1fr) 64px;gap:12px;font-size:12px;color:var(--ink-2);margin-bottom:8px}
.bars-h span:first-child{grid-column:1/3}
.bars-h .ax{display:none}
.brow{display:grid;grid-template-columns:minmax(120px,190px) minmax(0,1fr) 64px;gap:12px;align-items:center;padding:7px 0}
.bl{font-size:13px;text-align:right}
.bt{position:relative;height:18px;background:#EFECE5;border-radius:2px}
.bt::after{content:"";position:absolute;left:50%;top:-3px;bottom:-3px;width:1px;background:#D6D1C6}
.bt i{position:absolute;left:0;top:0;bottom:0;border-radius:2px;z-index:1}
.bt .ci{position:absolute;z-index:2;top:8px;height:2px;background:var(--ink);box-shadow:0 0 0 1.5px var(--halo)}
.bt .ci::before,.bt .ci::after{content:"";position:absolute;top:-4px;width:2px;height:10px;background:var(--ink);box-shadow:0 0 0 1.5px var(--halo)}
.bt .ci::before{left:0}.bt .ci::after{right:0}
.bv{font-size:13px;font-weight:600;text-align:right}
.rtsvg{max-width:900px}
.grp{display:flex;flex-wrap:wrap;align-items:center;gap:8px 14px}
.cap{font-size:12px;color:var(--ink-2);margin:6px 0 0}
.scroll{overflow-x:auto;-webkit-overflow-scrolling:touch}
.hint{display:none;font-size:12px;color:var(--ink-2);margin:4px 0 0}
.hint.show{display:block}
table{border-collapse:collapse;width:100%;font-size:13px}
th,td{padding:8px 8px;border-bottom:1px solid var(--line);text-align:right;white-space:nowrap}
th{white-space:normal;vertical-align:bottom;hyphens:manual;min-width:5.5ch}
th{font:600 11px/1.2 system-ui,sans-serif;letter-spacing:.05em;text-transform:uppercase;color:var(--ink-2);background:#FBFAF7}
td:first-child,th:first-child{text-align:left}
td{font-variant-numeric:tabular-nums}
td.num,.num{font-family:var(--mono);font-size:12.5px}
tr.me td{font-weight:600}
td.wrap{white-space:normal;overflow-wrap:anywhere;text-align:left}
.bar{display:flex;align-items:center;gap:8px;justify-content:flex-end}
.bar i{display:block;height:8px;border-radius:2px;min-width:1px}
.bar .track{width:90px;height:8px;background:#EFECE5;border-radius:2px;overflow:hidden}
.off{display:none!important}
.pending{display:flex;align-items:flex-start;gap:10px;border:1px dashed #D9C08F;background:#FDF8EE;color:var(--pending);border-radius:10px;padding:14px 16px;font-weight:600}
.pending .ph{margin-top:2px}
.simblk{border:1px solid #E7D3B0;border-radius:10px;padding:14px 16px;background:var(--surface)}
.note{font-size:12px;color:var(--ink-2)}
svg.chart{display:block;width:100%;height:auto}
svg.chart text{font-family:system-ui,-apple-system,"Segoe UI",Roboto,Arial,sans-serif}
svg.chart .t-num{font-family:ui-monospace,Menlo,Consolas,monospace}
dl.prov{display:grid;grid-template-columns:max-content 1fr;gap:6px 16px;margin:0;font-size:13px}
dl.prov dt{color:var(--ink-2)}
dl.prov dd{margin:0}
footer{max-width:1320px;margin:0 auto;padding:16px 24px 40px;color:var(--ink-2);font-size:12px;border-top:1px solid var(--line)}
footer p{margin:4px 0}
.fallback{display:none;margin-top:6px}
.fallback input{width:min(520px,100%);font:12px var(--mono);padding:6px;border:1px solid var(--line);border-radius:6px}
@media (min-width:1100px){.prov2{grid-template-columns:minmax(0,1fr) minmax(0,1fr)}}
@media (max-width:1279px){.wrap{grid-template-columns:minmax(0,1fr)}nav.index{display:none}.kpis{grid-template-columns:repeat(2,minmax(0,1fr))}
 .grid2{grid-template-columns:minmax(0,1fr)}}
@media (max-width:640px){.top{position:static}.top-in{padding:10px 14px}.wrap{padding:16px 14px 40px}.kpis{grid-template-columns:minmax(0,1fr)}
 .actions{margin-left:0;width:100%}.chartwrap{overflow-x:auto}.brow,.bars-h{grid-template-columns:96px minmax(0,1fr) 56px;gap:8px}.bl{font-size:12px}.chartwrap svg.chart{min-width:600px}.btn .lbl-long{display:none}.hint{display:block}.kpi .v{font-size:26px}.hero h1{font-size:30px}.hero .sub{font-size:15px}
 footer{padding:16px 14px 32px}}
@media print{.top,.controls,nav.index,.skip,.hint,.fallback{display:none!important}body{background:#fff}
 .wrap{display:block;padding:0}.card,.kpi{border-color:#ccc}section{break-before:page}section:first-of-type{break-before:auto}
 .scroll{overflow:visible}}
"""

# ---------------------------------------------------------------- charts

def success_chart_html(cond_key, arms_data):
    """Responsive HTML bars per arm: success rate, whisker = 95% CI. Rows carry data-arm so the page script can hide
    arms. role=img + aria-label summarise the chart for screen readers."""
    rows = [(k, arms_data.get(k)) for k in ARM_KEYS if isinstance(arms_data.get(k), dict)]
    summ = "; ".join(f"{ARM_LABEL[k]} {fmt_int(a.get('success'))} of {fmt_int(a.get('N'))}" for k, a in rows)
    p = [f'<div class="bars" role="img" aria-label="{esc(cond_key)} condition success: {esc(summ)}">'
         '<div class="bars-h"><span>Success rate · whisker: 95% CI</span><span class="ax"><i>0%</i><i>50%</i><i>100%</i></span></div>']
    for k, a in rows:
        rate, ci = a.get("success_rate"), a.get("success_ci95")
        w = f"{min(max(rate, 0), 1) * 100:.1f}%" if num(rate) else "0%"
        whisk = ""
        if isinstance(ci, (list, tuple)) and len(ci) >= 2 and num(ci[0]) and num(ci[1]):
            whisk = (f'<b class="ci" style="left:{ci[0] * 100:.1f}%;width:{(ci[1] - ci[0]) * 100:.1f}%"></b>')
        p.append(f'<div class="brow" data-arm="{k}"><span class="bl">{esc(ARM_LABEL[k])}</span>'
                 f'<span class="bt"><i style="width:{w};background:{ARM_COLOR[k]}"></i>{whisk}</span>'
                 f'<span class="bv num">{fmt_int(a.get("success"))}/{fmt_int(a.get("N"))}</span></div>')
    p.append('</div>')
    return "".join(p)


RT_COLORS = {"p50": "#8A8F98", "p99": "#4A4F59", "max": "#B42318"}


def rt_chart_svg(reports, aria):
    """Horizontal bars of p50/p99/max release latency per report, log scale (µs)."""
    groups, allv, dls = [], [], []
    for path, rep in reports:
        rl = rep.get("release_latency") if isinstance(rep, dict) else None
        rl = rl if isinstance(rl, dict) else {}
        vals = []
        for name, key in (("p50", "p50_ns"), ("p99", "p99_ns"), ("max", "max_ns")):
            v = rl.get(key)
            vals.append((name, v / 1000.0 if num(v) and v > 0 else None))
        if all(v is None for _n, v in vals):
            continue
        groups.append((os.path.basename(path), rep, vals))
        allv.extend(v for _n, v in vals if v is not None)
        dl = rl.get("deadline_ns")
        if num(dl) and dl > 0:
            dls.append(dl / 1000.0)
    if not groups:
        return f"<p class='note'>{DASH}</p>"
    allv = allv + dls
    lo = math.floor(math.log10(min(allv)))
    hi = math.ceil(math.log10(max(allv)))
    if hi <= lo:
        hi = lo + 1
    W, left, right = 720, 70, 620
    top, rowh, ghdr, ggap = 44, 18, 22, 14
    H = top + len(groups) * (ghdr + 3 * rowh + ggap) + 24

    def X(v):
        return left + (right - left) * (math.log10(max(v, 1e-9)) - lo) / (hi - lo)

    p = [f'<svg class="chart rtsvg" viewBox="0 0 {W} {H}" role="img" aria-label="{esc(aria)}">']
    lx = left
    for name in ("p50", "p99", "max"):
        p.append(f'<rect x="{lx}" y="8" width="10" height="10" rx="2" fill="{RT_COLORS[name]}"/>')
        p.append(f'<text x="{lx + 15}" y="17" font-size="12" fill="#4A4F59">{name}</text>')
        lx += 60
    p.append(f'<text x="{right}" y="17" font-size="12" text-anchor="end" fill="#4A4F59">µs, log scale</text>')
    for k in range(lo, hi + 1):
        x = X(10 ** k)
        p.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{H - 22}" stroke="#E3E0D8"/>')
        p.append(f'<text class="t-num" x="{x:.1f}" y="{H - 6}" font-size="11" text-anchor="middle" '
                 f'fill="#4A4F59">{10 ** k:g} µs</text>')
    if dls:
        x = X(dls[0])
        p.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{H - 22}" stroke="#B42318" '
                 f'stroke-dasharray="4 3"/>')
        p.append(f'<text x="{x + 4:.1f}" y="{top + 4}" font-size="11" fill="#B42318">deadline {dls[0]:g} µs</text>')
    for i, (fname, rep, vals) in enumerate(groups):
        y0 = top + i * (ghdr + 3 * rowh + ggap)
        p.append(f'<text x="0" y="{y0 + 14}" font-size="12" fill="#16181D">{esc(fname)} · '
                 f'{esc(raw(rep, "platform"))} · sched={esc(raw(rep, "sched"))}</text>')
        for j, (name, v) in enumerate(vals):
            y = y0 + ghdr + j * rowh
            p.append(f'<text x="{left - 8}" y="{y + 11}" font-size="11" text-anchor="end" fill="#4A4F59">{name}</text>')
            if v is None:
                continue
            p.append(f'<rect x="{left}" y="{y + 2}" width="{max(X(v) - left, 1.0):.1f}" height="11" rx="2" '
                     f'fill="{RT_COLORS[name]}"/>')
            p.append(f'<text class="t-num" x="{X(v) + 5:.1f}" y="{y + 12}" font-size="11" fill="#16181D">'
                     f'{v:.1f} µs</text>')
    p.append("</svg>")
    return "".join(p)


# ---------------------------------------------------------------- sections

def scroll_table(inner, caption):
    return (f'<div class="scroll" tabindex="0" role="region" aria-label="{esc(caption)}"><table>{inner}</table></div>'
            f'<p class="hint">Scroll the table sideways to see all columns.</p><p class="cap tcap">{esc(caption)}</p>')


def cond_table(arms_data):
    head = ("<tr><th>arm</th><th>success/N</th><th>rate (95% CI)</th><th>detected</th><th>recovered</th>"
            "<th>escalated</th><th>retries (same again)</th><th>mean steps</th>"
            "<th>vs none</th><th>McNemar&nbsp;p</th></tr>")
    rows = []
    for k in ARM_KEYS:
        a = arms_data.get(k)
        if not isinstance(a, dict):
            continue
        vs = a.get("vs_single_pass") or {}
        diff = vs.get("diff")
        diff_txt = DASH if not num(diff) else f"{diff * 100:+.1f} pp"
        steps = a.get("mean_sim_steps")
        mean0 = steps[0] if isinstance(steps, list) and steps else None
        rate_txt = pct(a.get("success_rate"))
        if isinstance(a.get("success_ci95"), (list, tuple)):
            rate_txt += f" ({ci_text(a.get('success_ci95'))})"
        rows.append(
            f'<tr data-arm="{k}"{" class=me" if k == "corr_aware_retry" else ""}>'
            f'<td><span class="sw" style="background:{ARM_COLOR[k]}"></span> {esc(ARM_LABEL[k])}</td>'
            f'<td class="num">{fmt_int(raw(a, "success"))}/{fmt_int(raw(a, "N"))}</td>'
            f'<td class="num">{esc(rate_txt)}</td>'
            f'<td class="num">{fmt_int(raw(a, "episodes_with_detected_failure"))}</td>'
            f'<td class="num">{fmt_int(raw(a, "recovered"))}</td>'
            f'<td class="num">{fmt_int(raw(a, "escalated_to_operator"))}</td>'
            f'<td class="num">{fmt_int(raw(a, "retries"))} ({fmt_int(raw(a, "retries_repeating_same_failure"))})</td>'
            f'<td class="num">{fmt(mean0)}</td>'
            f'<td class="num">{esc(diff_txt)}</td>'
            f'<td class="num">{esc(fmt_p(vs.get("mcnemar_p")))}</td></tr>')
    return head + "".join(rows)


def success_section(summary):
    conds = summary.get("conditions") or {}
    out = ['<section id="success" aria-labelledby="h-success">',
           f'<h2 id="h-success"><span class="n">01</span> {icon("chart-bar")} Success by condition and arm</h2>']
    for ck, clabel in CONDS:
        if ck not in conds:
            continue
        out.append(f'<div class="by-cond" data-cond="{ck}"><div class="stack">')
        out.append(f'<div class="card"><div class="chartwrap">{success_chart_html(ck, conds[ck])}</div>'
                   f'<p class="cap">{esc(clabel)} · {fmt_int(raw(summary, "N_per_arm"))} paired episodes per arm '
                   '(same seeds). Simulation results.</p></div>')
        out.append('<div class="card">' + scroll_table(
            cond_table(conds[ck]),
            f"{clabel}: success/N, rate with 95% CI, detected failures, recoveries, escalations, retries "
            "(of which repeated the same failure), mean sim steps, difference vs no middleware and McNemar p "
            "(paired, vs no middleware). Simulation results.") + '</div>')
        out.append('</div></div>')
    out.append('</section>')
    return "".join(out)


def fault_section(summary):
    field = (summary.get("conditions") or {}).get("field") or {}
    per_arm, faults = {}, []
    for k in ARM_KEYS:
        bf = raw(field.get(k) or {}, "by_fault")
        if isinstance(bf, dict):
            per_arm[k] = bf
            for f in bf:
                if f not in faults:
                    faults.append(f)
    out = ['<section id="faults" aria-labelledby="h-faults">',
           f'<h2 id="h-faults"><span class="n">02</span> {icon("lightning")} Field faults</h2><div class="card">']
    if not faults:
        out.append(f"<p class='note'>{DASH}</p></div></section>")
        return "".join(out)
    order = {"percep_bias": 0, "bump": 1, "slippery": 2, "none": 3}
    faults.sort(key=lambda f: (order.get(f, 9), f))
    head = "<tr><th>injected fault</th>" + "".join(
        f'<th data-arm="{k}">{esc(ARM_LABEL[k])}</th>' for k in ARM_KEYS) + "</tr>"
    rows = []
    for f in faults:
        cells = []
        for k in ARM_KEYS:
            e = (per_arm.get(k) or {}).get(f)
            if isinstance(e, dict) and num(e.get("N")) and num(e.get("success")) and e["N"]:
                r = e["success"] / e["N"]
                cells.append(f'<td data-arm="{k}"><div class="bar"><span class="num">{fmt_int(e["success"])}/'
                             f'{fmt_int(e["N"])}</span><span class="track"><i style="width:{r * 100:.1f}%;'
                             f'background:{ARM_COLOR[k]}"></i></span></div></td>')
            else:
                cells.append(f'<td data-arm="{k}">{DASH}</td>')
        rows.append(f"<tr><td>{esc(f)}</td>{''.join(cells)}</tr>")
    out.append(scroll_table(head + "".join(rows),
                            "Field condition: successes per injected fault and arm (from by_fault). "
                            "Perception bias is the correlated failure that naive retry cannot fix. Simulation results."))
    out.append('</div></section>')
    return "".join(out)


RT_HEAD = ("<tr><th>report</th><th>platform</th><th>sched</th><th>period</th><th>episodes</th><th>p50 µs</th>"
           "<th>p99 µs</th><th>p99.9 µs</th><th>max µs</th><th>misses &gt; deadline</th></tr>")


def rt_rows(reports):
    rows = []
    for path, rep in reports:
        rl = rep.get("release_latency") if isinstance(rep, dict) else None
        rl = rl if isinstance(rl, dict) else {}
        dm, cnt = rl.get("deadline_misses"), rl.get("count")
        miss = f"{fmt_int(dm)}/{fmt_int(cnt)}" if num(dm) and num(cnt) else DASH
        per = raw(rep, "period_ns")
        rows.append(f"<tr><td class='mono'>{esc(os.path.basename(path))}</td><td>{esc(raw(rep, 'platform'))}</td>"
                    f"<td>{esc(raw(rep, 'sched'))}</td><td class='num'>{us_of(per)} µs</td>"
                    f"<td class='num'>{fmt_int(raw(rep, 'episodes'))}</td>"
                    f"<td class='num'>{us_of(rl.get('p50_ns'))}</td><td class='num'>{us_of(rl.get('p99_ns'))}</td>"
                    f"<td class='num'>{us_of(rl.get('p999_ns'))}</td><td class='num'>{us_of(rl.get('max_ns'))}</td>"
                    f"<td class='num'>{esc(miss)}</td></tr>")
    return "".join(rows)


def rt_section(host_dir, host_reports, qnx_dir, qnx_reports, qnx_has_json, sim_dir, sim_reports):
    out = ['<section id="realtime" aria-labelledby="h-rt">',
           f'<h2 id="h-rt"><span class="n">03</span> {icon("timer")} Real-time control loop</h2>']
    out.append('<div class="stack"><div class="card">')
    out.append(f'<h3>Linux host baseline · <span class="mono">{esc(os.path.relpath(host_dir, ROOT))}</span></h3>')
    if host_reports:
        out.append('<div class="chartwrap">' + rt_chart_svg(host_reports, "Linux host release latency p50 p99 max per report, log scale") + '</div>')
        out.append(scroll_table(RT_HEAD + rt_rows(host_reports),
                                "Release latency of the 1 kHz control loop on a shared Linux machine (SCHED_OTHER). "
                                "Baseline for the QNX runs."))
    else:
        out.append(f"<p class='note'>{DASH} no rt_report*.json in the host directory</p>")
    out.append('</div><div class="card">')
    out.append(f'<h3>QNX 8.0 (QEMU x86_64){(" · <span class=mono>" + esc(qnx_dir) + "</span>") if qnx_dir else ""}</h3>')
    if qnx_dir is None or not qnx_has_json:
        out.append(f'<div class="pending">{icon("hourglass-medium")}<span>{esc(QNX_PENDING)}</span></div>')
        out.append('<p class="note">Filled only from results/qnx_qemu_x86_64/rt_report_*.json after '
                   'scripts/qnx_first_run.sh has run on QNX 8.0. Nothing here is estimated.</p>')
    elif qnx_reports:
        out.append('<div class="chartwrap">' + rt_chart_svg(qnx_reports, "QNX 8.0 release latency p50 p99 max per report, log scale") + '</div>')
        out.append(scroll_table(RT_HEAD + rt_rows(qnx_reports), "Release latency on QNX 8.0 in QEMU (emulator numbers)."))
    else:
        out.append(f"<p class='note'>{DASH} no rt_report*.json found in the QNX directory</p>")
    out.append('</div></div>')
    if sim_dir is not None:
        out.append(f'<div class="simblk" id="sim" style="margin-top:16px"><h3>{icon("flask")} {esc(SIM_LABEL)}</h3>')
        if sim_reports:
            out.append('<div class="chartwrap">' + rt_chart_svg(sim_reports, f"{SIM_LABEL}: release latency per report") + '</div>')
            out.append(scroll_table(RT_HEAD + rt_rows(sim_reports),
                                    f"{SIM_LABEL}. The timer is a Linux thread: these are Linux numbers, not QNX latency."))
        else:
            out.append(f"<p class='note'>{DASH} no rt_report*.json in {esc(sim_dir)} ({esc(SIM_LABEL)})</p>")
        out.append('</div>')
    out.append('</section>')
    return "".join(out)


def provenance_section(summary, inputs):
    out = ['<section id="provenance" aria-labelledby="h-prov">',
           f'<h2 id="h-prov"><span class="n">04</span> {icon("database")} Provenance</h2><div class="grid2 prov2"><div class="card">']
    ev, wu = raw(summary, "eval_seeds"), raw(summary, "warmup_seeds")
    rng = lambda s: f"{s[0]}–{s[1]}" if isinstance(s, list) and len(s) == 2 else DASH
    out.append('<dl class="prov">'
               f'<dt>Label</dt><dd>{esc(raw(summary, "label") or DASH)}</dd>'
               f'<dt>Episodes per arm</dt><dd class="num">{fmt_int(raw(summary, "N_per_arm"))} (paired, same seeds)</dd>'
               f'<dt>Evaluation seeds</dt><dd class="num">{esc(rng(ev))}</dd>'
               f'<dt>Library warm-up seeds</dt><dd class="num">{esc(rng(wu))} (library frozen before evaluation)</dd>'
               f'<dt>Library entries</dt><dd class="num">{fmt_int(raw(summary, "library_entries"))}</dd>'
               f'<dt>Policy bias std</dt><dd class="num">{fmt(raw(summary, "policy_bias_std"), 2)}</dd>'
               '<dt>Statistics</dt><dd>Wilson 95% intervals; McNemar exact test on paired episodes</dd>'
               '<dt>Reproduce</dt><dd><code class="mono">relmw_bench --n 100 --out results/host_linux</code></dd>'
               '</dl></div><div class="card">')
    rows = "".join(f"<tr><td class='mono wrap'>{esc(f['file'])}</td><td class='mono'>{esc(f['sha12'])}</td></tr>"
                   for f in inputs)
    out.append(scroll_table("<tr><th>input file</th><th>sha256[:12]</th></tr>" + rows,
                            "Every number on this page comes from these files."))
    out.append('</div></div></section>')
    return "".join(out)


def hero(summary, now):
    field = (summary.get("conditions") or {}).get("field") or {}
    mw, nv, sp = (field.get(k) or {} for k in ARM_KEYS[::-1])
    n = raw(summary, "N_per_arm")
    if num(mw.get("success")) and num(mw.get("N")):
        head = (f'relmw recovers <em>{fmt_int(mw.get("success"))} of {fmt_int(mw.get("N"))}</em> '
                'fault&#8209;injected robot episodes')
    else:
        head = "Reliability supervisor results"
    sub = (f'Same faults, same seeds: no middleware <b>{fmt_int(sp.get("success"))}</b>, naive retry '
           f'<b>{fmt_int(nv.get("success"))}</b>. QNX 8.0 numbers appear only after the first QEMU run.')
    return ('<section class="hero" aria-labelledby="h-hero">'
            f'<p class="eyebrow">{icon("flask")} Simulation · {fmt_int(n)} paired episodes per arm · not a physical robot</p>'
            f'<h1 id="h-hero">{head}</h1><p class="sub">{sub}</p>'
            f'<p class="meta">Generated <span class="mono">{esc(now)}</span> · failure library: '
            f'{fmt_int(raw(summary, "library_entries"))} entries</p></section>')


def kpis(summary, host_reports):
    field = (summary.get("conditions") or {}).get("field") or {}
    mw, nv, sp = (field.get(k) or {} for k in ARM_KEYS[::-1])
    vs = mw.get("vs_single_pass") or {}
    ci = mw.get("success_ci95")
    out = ['<div class="kpis" role="list">']
    def card(ico, title, value, lines):
        sub = "".join(f'<span class="ln{" nw" if nw else ""}">{t}</span>' for t, nw in lines)
        return (f'<div class="kpi" role="listitem"><div class="k">{icon(ico)} {title}</div>'
                f'<div class="v{" acc" if ico == "shield-check" else ""}">{value}</div><div class="s">{sub}</div></div>')
    NB = "&nbsp;"
    out.append(card("shield-check", "Field success · relmw", f'{fmt_int(mw.get("success"))}/{fmt_int(mw.get("N"))}',
                    [(f'95%{NB}CI{NB}{esc(ci_text(ci))}', True),
                     (f'naive{NB}<b>{fmt_int(nv.get("success"))}</b> · none{NB}<b>{fmt_int(sp.get("success"))}</b>', True)]))
    diff = vs.get("diff")
    out.append(card("chart-bar", "vs no middleware", "%+.0f pp" % (diff * 100) if num(diff) else DASH,
                    [(f'McNemar{NB}p{NB}={NB}<b>{esc(fmt_p(vs.get("mcnemar_p")))}</b>', True),
                     (f'fixed{NB}<b>{fmt_int(vs.get("fixed"))}</b> · broken{NB}<b>{fmt_int(vs.get("broken"))}</b>', True)]))
    out.append(card("arrow-counter-clockwise", "Same failure repeated",
                    f'{fmt_int(mw.get("retries_repeating_same_failure"))}<span style="font-size:16px;color:#4A4F59">'
                    f'/{fmt_int(mw.get("retries"))}</span>',
                    [("retries that failed the same way again", False),
                     (f'naive{NB}retry{NB}<b>{fmt_int(nv.get("retries_repeating_same_failure"))}/'
                      f'{fmt_int(nv.get("retries"))}</b>', True)]))
    p99 = mx = DASH
    if host_reports:
        rl = host_reports[0][1].get("release_latency") or {}
        p99, mx = us_of(rl.get("p99_ns")), us_of(rl.get("max_ns"))
    out.append(card("timer", "Host loop p99 · 1 kHz", f'{esc(p99)}<span style="font-size:16px;color:#4A4F59"> µs</span>',
                    [("Linux, SCHED_OTHER, shared host", False),
                     (f'max{NB}<b>{esc(mx)}{NB}µs</b> · QNX{NB}pending', True)]))
    out.append('</div>')
    return "".join(out)

JS = r"""
(function(){
'use strict';
var D = JSON.parse(document.getElementById('relmw-data').textContent);
var ARMS = ['single_pass','naive_retry','corr_aware_retry'];
var LABEL = {single_pass:'No middleware', naive_retry:'Naive retry', corr_aware_retry:'relmw (correlation-aware)'};
var COLOR = {single_pass:'#8A8F98', naive_retry:'#4A4F59', corr_aware_retry:'#C2410C'};
var CONDS = ['field','nominal'];
var body = document.body;
var st = {cond:'field', arms:ARMS.slice()};

function parse(){
  var q = new URLSearchParams(location.search);
  var c = q.get('cond'); st.cond = CONDS.indexOf(c) >= 0 ? c : 'field';
  var a = (q.get('arms') || '').split(',').filter(function(x){ return ARMS.indexOf(x) >= 0; });
  st.arms = a.length ? ARMS.filter(function(x){ return a.indexOf(x) >= 0; }) : ARMS.slice();
  return q.get('sec');
}
function url(){
  return location.href.split('#')[0].split('?')[0] + '?cond=' + st.cond + '&arms=' + st.arms.join(',');
}
function relayout(svg){
  var top = +svg.getAttribute('data-top'), rh = +svg.getAttribute('data-rowh'), foot = +svg.getAttribute('data-foot');
  var j = 0;
  svg.querySelectorAll('g.row').forEach(function(g){
    if (g.classList.contains('off')) return;
    g.setAttribute('transform', 'translate(0,' + (top + j * rh) + ')'); j++;
  });
  var h = top + Math.max(j, 1) * rh + foot;
  var vb = svg.getAttribute('viewBox').split(' '); vb[3] = h; svg.setAttribute('viewBox', vb.join(' '));
  var t = svg.querySelector('g.ticks'); if (t) t.setAttribute('transform', 'translate(0,' + (h - 8) + ')');
}
function apply(){
  body.setAttribute('data-cond', st.cond);
  body.setAttribute('data-arms', st.arms.join(','));
  document.querySelectorAll('.by-cond').forEach(function(el){ el.classList.toggle('off', el.getAttribute('data-cond') !== st.cond); });
  document.querySelectorAll('[data-arm]').forEach(function(el){ el.classList.toggle('off', st.arms.indexOf(el.getAttribute('data-arm')) < 0); });
  document.querySelectorAll('svg.succ').forEach(relayout);
  document.querySelectorAll('[data-set-cond]').forEach(function(b){ b.setAttribute('aria-pressed', String(b.getAttribute('data-set-cond') === st.cond)); });
  document.querySelectorAll('input[data-toggle-arm]').forEach(function(i){ i.checked = st.arms.indexOf(i.getAttribute('data-toggle-arm')) >= 0; });
  try { history.replaceState(null, '', url()); } catch (e) { /* some file:// contexts refuse; the view still updates */ }
}
function say(msg){ var s = document.getElementById('status'); s.textContent = msg; clearTimeout(say.t); say.t = setTimeout(function(){ s.textContent = ''; }, 4000); }
function download(name, type, data){
  var a = document.createElement('a'); var u = URL.createObjectURL(data instanceof Blob ? data : new Blob([data], {type:type}));
  a.href = u; a.download = name; document.body.appendChild(a); a.click(); a.remove(); setTimeout(function(){ URL.revokeObjectURL(u); }, 2000);
}
function pct(x){ return (x * 100).toFixed(1) + '%'; }
function ci(c){ return c && c.length > 1 ? (c[0] * 100).toFixed(1) + '–' + (c[1] * 100).toFixed(1) + '%' : '–'; }
function fp(p){ return typeof p !== 'number' ? '–' : (p < 0.001 ? p.toExponential(1) : String(+p.toPrecision(3))); }
function us(ns){ return typeof ns === 'number' ? (ns / 1000).toFixed(1) : '–'; }

function report(){
  var S = D.summary, C = S.conditions[st.cond] || {}, F = S.conditions.field || {};
  var L = [];
  L.push('# ' + D.title, '', '> ' + S.label, '', 'Generated ' + D.generated + ' · view: condition `' + st.cond + '`, arms `' + st.arms.join(',') + '`', '');
  L.push('## Success · ' + st.cond + ' condition (' + S.N_per_arm + ' paired episodes per arm)', '',
         '| arm | success/N | rate (95% CI) | recovered | escalated | retries (same failure again) | vs no middleware | McNemar p |',
         '|---|---|---|---|---|---|---|---|');
  st.arms.forEach(function(k){ var a = C[k]; if (!a) return; var v = a.vs_single_pass || {};
    L.push('| ' + LABEL[k] + ' | ' + a.success + '/' + a.N + ' | ' + pct(a.success_rate) + ' (' + ci(a.success_ci95) + ') | ' + a.recovered + ' | ' +
      a.escalated_to_operator + ' | ' + a.retries + ' (' + a.retries_repeating_same_failure + ') | ' +
      (typeof v.diff === 'number' ? (v.diff >= 0 ? '+' : '') + (v.diff * 100).toFixed(1) + ' pp' : '–') + ' | ' + fp(v.mcnemar_p) + ' |'); });
  L.push('', '## Field faults (successes per injected fault)', '', '| fault | ' + st.arms.map(function(k){ return LABEL[k]; }).join(' | ') + ' |',
         '|---|' + st.arms.map(function(){ return '---'; }).join('|') + '|');
  var faults = Object.keys(((F.single_pass || {}).by_fault) || {});
  faults.forEach(function(f){ L.push('| ' + f + ' | ' + st.arms.map(function(k){ var e = ((F[k] || {}).by_fault || {})[f]; return e ? e.success + '/' + e.N : '–'; }).join(' | ') + ' |'); });
  L.push('', '## Real-time control loop (release latency, µs)', '', '| report | platform | sched | p50 | p99 | p99.9 | max | misses |', '|---|---|---|---|---|---|---|---|');
  D.host_reports.forEach(function(r){ var l = r.release_latency || {};
    L.push('| ' + r.file + ' | ' + r.platform + ' | ' + r.sched + ' | ' + us(l.p50_ns) + ' | ' + us(l.p99_ns) + ' | ' + us(l.p999_ns) + ' | ' + us(l.max_ns) + ' | ' + l.deadline_misses + '/' + l.count + ' |'); });
  L.push('', 'QNX 8.0 (QEMU): ' + (D.qnx_reports.length ? D.qnx_reports.length + ' report(s) included above in the page' : D.qnx_pending));
  if (D.sim_reports) L.push('', D.sim_label + ': ' + D.sim_reports.length + ' report(s). Linux numbers, not QNX latency.');
  L.push('', '## Inputs (sha256[:12])', '');
  D.inputs.forEach(function(f){ L.push('- `' + f.file + '` ' + f.sha12); });
  L.push('', 'Generated by tools/make_dashboard.py · MIT · elidot', '');
  return L.join('\n');
}

function esc(s){ return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }
function cardSvg(){
  var S = D.summary, C = S.conditions[st.cond] || {}, F = (S.conditions.field || {}), mw = F.corr_aware_retry || {}, v = mw.vs_single_pass || {};
  var W = 1200, H = 630, s = [];
  var font = 'font-family="system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif"';
  var mono = 'font-family="ui-monospace,Menlo,Consolas,monospace"';
  s.push('<svg xmlns="' + 'http://www.w3.org/2000/svg' + '" width="' + W + '" height="' + H + '" viewBox="0 0 ' + W + ' ' + H + '">');
  s.push('<rect width="100%" height="100%" fill="#F6F4EF"/><rect x="0" y="0" width="12" height="630" fill="#C2410C"/>');
  s.push('<text x="64" y="84" ' + font + ' font-size="22" font-weight="600" fill="#4A4F59">elidot · relmw-qnx</text>');
  s.push('<text x="64" y="132" ' + font + ' font-size="40" font-weight="700" fill="#16181D">Reliability supervisor results</text>');
  s.push('<rect x="64" y="156" width="250" height="34" rx="17" fill="#FBF3E4" stroke="#E7D3B0"/><text x="189" y="179" text-anchor="middle" ' + font + ' font-size="15" font-weight="700" fill="#9A6700">SIMULATION RESULTS</text>');
  s.push('<text x="64" y="240" ' + font + ' font-size="18" fill="#4A4F59">Field faults · relmw success</text>');
  s.push('<text x="64" y="300" ' + mono + ' font-size="56" font-weight="700" fill="#C2410C">' + mw.success + '/' + mw.N + '</text>');
  s.push('<text x="64" y="336" ' + font + ' font-size="17" fill="#4A4F59">vs no middleware ' + (typeof v.diff === 'number' ? '+' + (v.diff * 100).toFixed(0) + ' pp' : '–') + ' · McNemar p = ' + fp(v.mcnemar_p) + '</text>');
  s.push('<text x="560" y="240" ' + font + ' font-size="18" fill="#4A4F59">Success rate · ' + esc(st.cond) + ' condition · 95% CI</text>');
  var y = 270, x0 = 760, x1 = 1110;
  st.arms.forEach(function(k){ var a = C[k]; if (!a) return;
    s.push('<text x="' + (x0 - 14) + '" y="' + (y + 19) + '" text-anchor="end" ' + font + ' font-size="17" fill="#16181D">' + esc(LABEL[k]) + '</text>');
    s.push('<rect x="' + x0 + '" y="' + y + '" width="' + (x1 - x0) + '" height="26" rx="3" fill="#EFECE5"/>');
    s.push('<rect x="' + x0 + '" y="' + y + '" width="' + Math.max(1, (x1 - x0) * a.success_rate) + '" height="26" rx="3" fill="' + COLOR[k] + '"/>');
    s.push('<text x="' + (x0 + 10) + '" y="' + (y + 19) + '" ' + mono + ' font-size="16" font-weight="700" fill="#FFFFFF">' + a.success + '/' + a.N + '</text>');
    y += 50; });
  s.push('<line x1="64" y1="470" x2="1136" y2="470" stroke="#E3E0D8"/>');
  s.push('<text x="64" y="512" ' + font + ' font-size="16" fill="#4A4F59">' + esc(S.label) + '</text>');
  s.push('<text x="64" y="544" ' + font + ' font-size="16" fill="#4A4F59">' + S.N_per_arm + ' paired episodes per arm · QNX 8.0 numbers: ' + (D.qnx_reports.length ? 'see dashboard' : 'pending first run in QEMU') + '</text>');
  s.push('<text x="64" y="590" ' + font + ' font-size="14" fill="#8A8F98">Generated ' + esc(D.generated) + ' · MIT · elidot</text>');
  s.push('</svg>');
  return s.join('');
}
function exportPng(){
  var img = new Image();
  img.onload = function(){
    var c = document.createElement('canvas'); c.width = 1200; c.height = 630;
    var g = c.getContext('2d'); g.drawImage(img, 0, 0);
    c.toBlob(function(b){ if (b) { download('relmw-summary-' + st.cond + '.png', 'image/png', b); say('PNG exported'); } else say('PNG export failed'); }, 'image/png');
  };
  img.onerror = function(){ say('PNG export failed'); };
  img.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(cardSvg());
}
function copyLink(){
  var u = url();
  var fb = function(){ var f = document.getElementById('fallback'); var i = f.querySelector('input'); i.value = u; f.style.display = 'block'; i.focus(); i.select(); say('Copy the selected link'); };
  if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(u).then(function(){ say('Link copied'); }, fb); else fb();
}

var sec = parse();
document.querySelectorAll('[data-set-cond]').forEach(function(b){ b.addEventListener('click', function(){ st.cond = b.getAttribute('data-set-cond'); apply(); }); });
document.querySelectorAll('input[data-toggle-arm]').forEach(function(i){ i.addEventListener('change', function(){
  var k = i.getAttribute('data-toggle-arm');
  var next = ARMS.filter(function(x){ return x === k ? i.checked : st.arms.indexOf(x) >= 0; });
  if (!next.length) { i.checked = true; say('Keep at least one arm'); return; }
  st.arms = next; apply(); }); });
document.getElementById('a-copy').addEventListener('click', copyLink);
var sh = document.getElementById('a-share');
if (navigator.share) { sh.hidden = false; sh.addEventListener('click', function(){ navigator.share({title: document.title, url: url()}).catch(function(){}); }); }
document.getElementById('a-md').addEventListener('click', function(){ download('relmw-report-' + st.cond + '.md', 'text/markdown;charset=utf-8', report()); say('Report downloaded'); });
document.getElementById('a-json').addEventListener('click', function(){ download('relmw-data.json', 'application/json', JSON.stringify(D, null, 2)); say('Data downloaded'); });
document.getElementById('a-png').addEventListener('click', exportPng);
document.getElementById('a-print').addEventListener('click', function(){ window.print(); });
document.querySelectorAll('nav.index a').forEach(function(a){ a.addEventListener('click', function(){
  try { history.replaceState(null, '', url() + '&sec=' + a.getAttribute('href').slice(1)); } catch (e) {} }); });
var navA = document.querySelectorAll('nav.index a');
if ('IntersectionObserver' in window && navA.length) {
  var vis = {};
  var io = new IntersectionObserver(function(es){
    es.forEach(function(e){ vis[e.target.id] = e.isIntersecting; });
    var cur = null;
    navA.forEach(function(a){ var id = a.getAttribute('href').slice(1); if (!cur && vis[id]) cur = id; });
    if (cur) navA.forEach(function(a){ a.setAttribute('aria-current', String(a.getAttribute('href') === '#' + cur)); });
  }, {rootMargin: '-120px 0px -55% 0px'});
  navA.forEach(function(a){ var el = document.getElementById(a.getAttribute('href').slice(1)); if (el) io.observe(el); });
}
apply();
if (sec && /^[a-z]+$/.test(sec) && document.getElementById(sec)) {
  try { history.replaceState(null, '', url() + '&sec=' + sec); } catch (e) {}
  document.getElementById(sec).scrollIntoView();
}
body.setAttribute('data-ready', '1');
body.setAttribute('data-scroll-width', String(document.documentElement.scrollWidth));
var clipped = [];
document.querySelectorAll('.scroll').forEach(function(el, i){ if (el.offsetParent && el.scrollWidth > el.clientWidth + 1) { clipped.push(el.getAttribute('aria-label') || String(i)); var h = el.nextElementSibling; if (h && h.classList.contains('hint')) h.classList.add('show'); } });
body.setAttribute('data-clipped-tables', String(clipped.length));
body.setAttribute('data-clipped-list', clipped.join(' | '));
var kov = 0;
document.querySelectorAll('.kpi .ln.nw').forEach(function(el){ if (el.scrollWidth > el.clientWidth + 1) kov++; });
body.setAttribute('data-kpi-overflow', String(kov));
})();
"""


def build(host_dir, qnx_dir, sim_dir=None):
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
    sim_reports = []
    if sim_dir:
        for path in sorted(glob.glob(os.path.join(sim_dir, "rt_report*.json"))):
            sim_reports.append((path, load_json(path)))
            inputs.append(path)

    now = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    rel = lambda p: os.path.relpath(p, ROOT) if os.path.abspath(p).startswith(ROOT + os.sep) else os.path.basename(p)
    in_list = [{"file": rel(p), "sha12": sha12(p)} for p in inputs]
    pack = lambda reps: [dict(rep, file=os.path.basename(p)) for p, rep in reps]
    data = {"title": TITLE, "generated": now, "summary": summary, "host_reports": pack(host_reports),
            "qnx_reports": pack(qnx_reports),
            "qnx_pending": QNX_PENDING if (qnx_dir is None or not qnx_has_json) else None, "inputs": in_list}
    if sim_dir is not None:
        data["sim_label"] = SIM_LABEL
        data["sim_reports"] = pack(sim_reports)
    data_json = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
    label = raw(summary, "label") or DASH
    arms_attr = ",".join(ARM_KEYS)

    b = []
    b.append('<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">'
             '<meta name="viewport" content="width=device-width,initial-scale=1">'
             f'<title>{esc(TITLE)}</title><meta name="description" content="Simulation results of relmw-qnx, a '
             'real-time reliability supervisor for robot manipulation built for QNX 8.0.">'
             f'<style>{CSS}</style></head>')
    b.append(f'<body data-cond="field" data-arms="{arms_attr}">')
    b.append('<a class="skip" href="#results">Skip to results</a>')
    b.append('<header class="top"><div class="top-in">'
             f'<div class="brand"><span class="mark">{icon("pulse")}</span>elidot <span class="dim">· relmw-qnx</span></div>'
             f'<span class="pill" title="{esc(label)}">{icon("flask")} Simulation results</span>'
             '<div class="actions" role="toolbar" aria-label="Share and export">'
             f'<button class="btn primary" id="a-copy" type="button">{icon("link")}Copy link</button>'
             f'<button class="btn" id="a-share" type="button" hidden>{icon("share-network")}Share</button>'
             f'<button class="btn" id="a-md" type="button">{icon("file-text")}Report<span class="lbl-long">&nbsp;.md</span></button>'
             f'<button class="btn" id="a-json" type="button">{icon("download-simple")}Data<span class="lbl-long">&nbsp;.json</span></button>'
             f'<button class="btn" id="a-png" type="button">{icon("image")}PNG</button>'
             f'<button class="btn" id="a-print" type="button">{icon("printer")}Print</button>'
             '</div></div></header>')
    b.append('<div class="wrap"><nav class="index" aria-label="Sections"><ol>'
             '<li><a href="#success"><span class="n">01</span>Success</a></li>'
             '<li><a href="#faults"><span class="n">02</span>Field faults</a></li>'
             '<li><a href="#realtime"><span class="n">03</span>Real-time</a></li>'
             '<li><a href="#provenance"><span class="n">04</span>Provenance</a></li></ol></nav>')
    b.append('<main id="results" tabindex="-1">')
    b.append(hero(summary, now))
    b.append('<p id="status" class="status" role="status" aria-live="polite"></p>'
             '<div id="fallback" class="fallback"><label class="note" for="fb-url">Link to this view</label><br>'
             '<input id="fb-url" readonly></div>')
    b.append(kpis(summary, host_reports))
    b.append('<div class="controls" aria-label="View"><div class="grp"><span class="lab">Condition</span>'
             '<div class="seg" role="group" aria-label="Condition">'
             + "".join(f'<button type="button" data-set-cond="{ck}" aria-pressed="{str(ck == "field").lower()}">'
                       f'{esc(cl)}</button>' for ck, cl in CONDS)
             + '</div></div><div class="grp" role="group" aria-label="Arms"><span class="lab">Arms</span>'
             + "".join(f'<label class="chk"><input type="checkbox" data-toggle-arm="{k}" checked>'
                       f'<span class="sw" style="background:{ARM_COLOR[k]}"></span>{esc(ARM_LABEL[k])}</label>'
                       for k in ARM_KEYS)
             + '</div></div>')
    b.append(success_section(summary))
    b.append(fault_section(summary))
    b.append(rt_section(host_dir, host_reports, qnx_dir, qnx_reports, qnx_has_json, sim_dir, sim_reports))
    b.append(provenance_section(summary, in_list))
    b.append('</main></div>')
    b.append('<footer><p>Generated by <span class="mono">tools/make_dashboard.py</span> from the input files listed '
             'under Provenance. One self-contained file: no external requests.</p>'
             '<p>relmw-qnx · MIT License · Copyright (c) 2026 elidot · Icons: Phosphor (MIT)</p></footer>')
    b.append(f'<script type="application/json" id="relmw-data">{data_json}</script>')
    b.append(f'<script>{JS}</script></body></html>')
    return "\n".join(b) + "\n"


def main():
    ap = argparse.ArgumentParser(description="Build the self-contained one-page results dashboard.")
    ap.add_argument("--host", required=True, help="host results directory (summary.json + rt_report*.json)")
    ap.add_argument("--qnx", help="optional QNX results directory (rt_report*.json)")
    ap.add_argument("--sim", help="optional QNX API simulation results directory (rt_report*.json); labelled "
                                  "'QNX API simulation on Linux, not QNX'")
    ap.add_argument("--out", required=True, help="output HTML file")
    args = ap.parse_args()
    html_text = build(args.host, args.qnx, args.sim)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(html_text)
    print(f"wrote {args.out} ({len(html_text)} bytes)")


if __name__ == "__main__":
    main()
