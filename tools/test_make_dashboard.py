#!/usr/bin/env python3
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
            self.assertNotIn("http://", page)
            self.assertNotIn("https://", page)
            self.assertNotIn("<link", page)

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
