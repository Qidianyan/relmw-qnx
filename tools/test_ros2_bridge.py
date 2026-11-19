#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
"""Unit & integration tests for tools/ros2_qnx_bridge.py (128-byte binary frame parity, UDP bridge, ROS 2 envelope)."""

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ros2_qnx_bridge as bridge  # noqa: E402


class Ros2QnxBridgeTest(unittest.TestCase):
    def test_128_byte_pack_decode_and_crc_guard(self):
        raw = bridge.pack_frame({
            "flags": 0x03,
            "seq": 17,
            "episode": 1004,
            "timestamp_ns": 5_000_000,
            "release_latency_ns": 14_200,
            "pusher_xy": (0.045, -0.19),
            "desired_xy": (0.055, -0.17),
            "safe_xy": (0.021, -0.21),
            "obj_xy": (0.12, -0.02, -0.12, -0.02),
            "goal_xy": (0.25, 0.20, -0.25, 0.20),
            "min_clearance_m": 0.0065,
            "compute_us": 2.4,
            "shield_interventions": 5,
            "robot_reconnects": 1,
            "shield_reason_mask": 0x04,
            "arm_id": 2,
            "strategy_id": 2,
            "failure_mode": 4,
            "subtask_id": 0,
            "attempt": 1,
            "policy_kind": 1,
            "tick": 88,
        })
        self.assertEqual(len(raw), 128)
        d = bridge.decode_frame(raw)
        self.assertEqual(d["seq"], 17)
        self.assertEqual(d["episode"], 1004)
        self.assertEqual(d["tick"], 88)
        self.assertEqual(d["arm"], "corr_aware_retry")
        self.assertEqual(d["strategy"], "relocalize")
        self.assertEqual(d["failure_mode"], "off_axis_drift")
        self.assertEqual(d["policy"], "tiny_mlp_fp32")
        self.assertEqual(d["attempt"], 1)
        self.assertEqual(d["shield_interventions"], 5)
        self.assertEqual(d["robot_reconnects"], 1)
        self.assertTrue(d["shield_intervened"])

        ros_msg = bridge.to_ros2_diagnostic(d)
        self.assertEqual(ros_msg["topic"], "/relmw/diagnostics")
        self.assertEqual(ros_msg["status"]["hardware_id"], "qnx80_relmw_cell")

        # Flip 1 bit -> ValueError on CRC-32
        bad = bytearray(raw)
        bad[50] ^= 0x80
        with self.assertRaises(ValueError):
            bridge.decode_frame(bytes(bad))

    def test_decode_binary_file_cli(self):
        with tempfile.TemporaryDirectory() as td:
            bpath = os.path.join(td, "sample.bin")
            with open(bpath, "wb") as fh:
                for k in range(5):
                    fh.write(bridge.pack_frame({"seq": k, "episode": 1000, "tick": k * 5, "shield_interventions": k}))
            proc = subprocess.run(
                [sys.executable, os.path.join(ROOT, "tools", "ros2_qnx_bridge.py"), "decode", "--bin", bpath],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            rows = [json.loads(line) for line in proc.stdout.strip().splitlines()]
            self.assertEqual(len(rows), 5)
            self.assertEqual([r["seq"] for r in rows], [0, 1, 2, 3, 4])

    def test_live_udp_listener(self):
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rx.bind(("127.0.0.1", 0))
        port = rx.getsockname()[1]
        rx.close()

        out_lines = []

        def run_listener():
            out_lines.extend(bridge.listen_udp("127.0.0.1", port, max_frames=3, timeout_s=3.0))

        th = threading.Thread(target=run_listener, daemon=True)
        th.start()

        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        import time
        time.sleep(0.08)
        for k in range(3):
            tx.sendto(bridge.pack_frame({"seq": 10 + k, "tick": k, "episode": 1010}), ("127.0.0.1", port))
        tx.close()
        th.join(timeout=3.0)
        self.assertEqual(len(out_lines), 3)
        self.assertEqual([r["seq"] for r in out_lines], [10, 11, 12])


if __name__ == "__main__":
    unittest.main()
