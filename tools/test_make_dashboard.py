#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
"""Tests for tools/make_dashboard.py (registered in ctest as test_dashboard).

Runs the real generator against the real results/host_linux, writing output
to a temp directory, and checks:
  * the field table shows 78 / 80 / 98 and the SIMULATION ONLY label;
  * the QNX pending placeholder appears without --qnx and disappears (with a
    QNX row present) when a fake QNX rt_report directory is given;
  * the page is fully offline (no <script src>, no http(s)://, no <link>);
  * the HTML parses cleanly and every opened <svg> is closed.
"""

import html.parser
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(ROOT, "tools", "make_dashboard.py")
HOST = os.path.join(ROOT, "results", "host_linux")
QNX_PENDING = "【待 QNX 实测】 (pending first run on QNX 8.0 in QEMU)"


class SvgCounter(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.svg_open = 0
        self.svg_close = 0

    def handle_starttag(self, tag, attrs):
        if tag == "svg":
            self.svg_open += 1

    def handle_endtag(self, tag):
        if tag == "svg":
            self.svg_close += 1


class DashboardTest(unittest.TestCase):
    def run_gen(self, out, qnx=None):
        cmd = [sys.executable, TOOL, "--host", HOST, "--out", out]
        if qnx is not None:
            cmd += ["--qnx", qnx]
        proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        with open(out, encoding="utf-8") as fh:
            return fh.read()

    def test_field_table_and_label(self):
        with tempfile.TemporaryDirectory() as td:
            out = os.path.join(td, "dashboard.html")
            page = self.run_gen(out)
            self.assertTrue(os.path.exists(out))
            self.assertIn("SIMULATION ONLY", page)
            # field condition table: 78 / 80 / 98 successes
            for token in ("78/100", "80/100", "98/100"):
                self.assertIn(token, page)
            self.assertIn("field", page)
            self.assertIn("relmw-qnx — reliability supervisor results", page)

    def test_latency_chart_labels_in_microseconds(self):
        # regression: chart labels once divided µs by 1000 again ("0.1 µs" for a 60 µs p50)
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "d.html"))
            self.assertIn(">60.4 µs<", page)
            self.assertIn(">3138.5 µs<", page)
            self.assertNotIn(">0.1 µs<", page)

    def test_qnx_placeholder_without_dir(self):
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "dashboard.html"))
            self.assertIn(QNX_PENDING, page)

    def test_qnx_row_with_fake_dir(self):
        with tempfile.TemporaryDirectory() as td:
            qdir = os.path.join(td, "qnx")
            os.makedirs(qdir)
            src = os.path.join(HOST, "rt_report_sched_other_1khz.json")
            with open(src, encoding="utf-8") as fh:
                rep = json.load(fh)
            rep["platform"] = "QNX"
            dst = os.path.join(qdir, "rt_report_qnx_1khz.json")
            with open(dst, "w", encoding="utf-8") as fh:
                json.dump(rep, fh)
            page = self.run_gen(os.path.join(td, "dashboard.html"), qnx=qdir)
            self.assertNotIn(QNX_PENDING, page)
            self.assertIn(">QNX<", page)

    def test_fully_offline(self):
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "dashboard.html"))
            self.assertNotIn("<script src", page)
            self.assertNotIn("<link", page)
            self.assertNotIn("@import", page)
            self.assertIsNone(re.search(r"url\(\s*['\"]?(https?:|//)", page))
            # the only URL allowed is the SVG namespace (used when the PNG card is serialized)
            urls = set(re.findall(r"https?://[^\s\"'<>)]*", page))
            self.assertLessEqual(urls, {"http://www.w3.org/2000/svg"}, urls)

    def test_embedded_json_matches_summary(self):
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "d.html"))
            m = re.search(r'<script type="application/json" id="relmw-data">(.*?)</script>', page, re.S)
            self.assertIsNotNone(m)
            data = json.loads(m.group(1))
            with open(os.path.join(HOST, "summary.json"), encoding="utf-8") as fh:
                summary = json.load(fh)
            self.assertEqual(data["summary"], summary)
            f = data["summary"]["conditions"]["field"]
            self.assertEqual([f[k]["success"] for k in ("single_pass", "naive_retry", "corr_aware_retry")], [78, 80, 98])
            self.assertEqual(data["qnx_reports"], [])
            self.assertEqual(data["qnx_pending"], QNX_PENDING)
            self.assertTrue(all(len(x["sha12"]) == 12 for x in data["inputs"]))

    def test_kpis_from_data(self):
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "d.html"))
            self.assertIn('class="v acc">98/100<', page)
            self.assertIn(">+20 pp<", page)
            self.assertIn("1.9e-06", page)          # McNemar p corr_aware vs single_pass, as in summary.json
            self.assertIn(">262.1<", page)          # host p99 (SCHED_OTHER 1 kHz)

    def test_hero_and_kpi_typography(self):
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "d.html"))
            self.assertIn("relmw recovers <em>98 of 100</em>", page)
            self.assertRegex(page, r'class="eyebrow">.*Simulation')
            self.assertLess(page.index('class="hero"'), page.index('class="kpis"'))
            self.assertLess(page.index('class="kpis"'), page.index('class="controls"'))
            kv = re.search(r"\.kpi \.v\{([^}]*)\}", page).group(1)
            self.assertNotIn("var(--mono)", kv)
            self.assertIn("tabular-nums", kv)
            self.assertIn("aria-current", page)

    def test_only_vendored_phosphor_icons(self):
        icon_dir = os.path.join(ROOT, "tools", "dashboard_assets", "phosphor", "regular")
        vendored = set()
        for name in os.listdir(icon_dir):
            with open(os.path.join(icon_dir, name), encoding="utf-8") as fh:
                vendored.update(re.findall(r'<path d="([^"]+)"', fh.read()))
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "d.html"))
            icons = re.findall(r'<svg class="ph"[^>]*>(.*?)</svg>', page, re.S)
            self.assertGreaterEqual(len(icons), 10)
            for inner in icons:
                paths = re.findall(r'<path d="([^"]+)"', inner)
                self.assertTrue(paths)
                for d in paths:
                    self.assertIn(d, vendored)
                self.assertNotIn("<img", inner)

    def test_no_emoji_and_brand(self):
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "d.html"))
            bad = [c for c in page if 0x1F000 <= ord(c) <= 0x1FAFF or 0x2600 <= ord(c) <= 0x27BF]
            self.assertEqual(bad, [])
            self.assertIn("elidot", page)
            for word in ("有限公司", "Co., Ltd", "Inc.", "LLC", "GmbH"):
                self.assertNotIn(word, page)

    def test_function_paths_present(self):
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "d.html"))
            for needle in ('id="a-copy"', 'id="a-share"', 'id="a-md"', 'id="a-json"', 'id="a-png"', 'id="a-print"',
                           "history.replaceState", "navigator.clipboard", "toBlob", "window.print",
                           'data-set-cond="nominal"', 'data-toggle-arm="naive_retry"', 'href="#results"',
                           "@media print", 'aria-live="polite"'):
                self.assertIn(needle, page)

    def test_sim_block_labelled(self):
        with tempfile.TemporaryDirectory() as td:
            sdir = os.path.join(td, "sim")
            os.makedirs(sdir)
            with open(os.path.join(HOST, "rt_report_sched_other_1khz.json"), encoding="utf-8") as fh:
                rep = json.load(fh)
            rep["platform"] = "QNX API simulation on Linux (not QNX)"
            with open(os.path.join(sdir, "rt_report_sim_1khz.json"), "w", encoding="utf-8") as fh:
                json.dump(rep, fh)
            cmd = [sys.executable, TOOL, "--host", HOST, "--sim", sdir, "--out", os.path.join(td, "d.html")]
            proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            page = open(os.path.join(td, "d.html"), encoding="utf-8").read()
            m = re.search(r'<div class="simblk".*?class="cap tcap">.*?</p>', page, re.S)
            self.assertIsNotNone(m)
            blk = m.group(0)
            self.assertIn("QNX API simulation on Linux, not QNX</h3>", blk)                     # heading
            self.assertIn('aria-label="QNX API simulation on Linux, not QNX: release latency', blk)   # chart
            self.assertIn('<p class="cap tcap">QNX API simulation on Linux, not QNX.', blk)        # table caption
            self.assertIn(QNX_PENDING, page)   # never merged with QNX numbers

    def test_html_parses_and_svg_closed(self):
        with tempfile.TemporaryDirectory() as td:
            page = self.run_gen(os.path.join(td, "dashboard.html"))
            counter = SvgCounter()
            try:
                counter.feed(page)
                counter.close()
            except Exception as exc:  # noqa: BLE001 - any parse error fails the test
                self.fail(f"html.parser raised: {exc!r}")
            self.assertGreater(counter.svg_open, 0)
            self.assertEqual(counter.svg_open, counter.svg_close)
            self.assertEqual(page.count("<svg"), page.count("</svg>"))


if __name__ == "__main__":
    unittest.main()
