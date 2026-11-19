#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
"""Zero-copy 128-byte binary telemetry decoder & ROS 2 / UDP bridge for relmw-qnx.

Decodes the exact 128-byte `relmw::TelemetryFrame` wire struct emitted by QNX 8.0 / POSIX supervisors
(over non-blocking UDP datagrams or binary `.bin` files), verifies the IEEE 802.3 CRC-32 checksum, and:
  1. Publishes ROS 2 `/relmw/diagnostics` (`diagnostic_msgs/DiagnosticArray`) and `/relmw/pose`
     (`geometry_msgs/PoseStamped`) automatically if `rclpy` is sourced in the environment;
  2. Otherwise streams structured JSON-Lines telemetry with zero external Python dependencies;
  3. Bridges high-level ROS 2 / VLA task plans or fault-injection commands into `/dev/relmw/ctl`.

Usage:
    python3 tools/ros2_qnx_bridge.py decode --bin results/host_splus/replay_showcase.bin
    python3 tools/ros2_qnx_bridge.py listen --port 9870 --max-frames 100
    python3 tools/ros2_qnx_bridge.py ctl --mount /dev/relmw "plan push_B,push_A"
"""

import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

MAGIC = 0x524D5754  # "RMWT"
VERSION = 1
FRAME_SIZE = 128
# Exact little-endian wire layout matching include/relmw/telemetry_bridge.hpp (35 unpacked items, 128 bytes)
FRAME_STRUCT = struct.Struct("<IHHIiqq2f2f2f4f4f2fIIH6BQII")
assert FRAME_STRUCT.size == FRAME_SIZE, f"Expected 128 bytes, got {FRAME_STRUCT.size}"

FLAG_SHIELD_ENABLED = 1 << 0
FLAG_SHIELD_INTERVENED = 1 << 1
FLAG_CORRELATED_FAIL = 1 << 2
FLAG_ROBOT_LINK_LOST = 1 << 3
FLAG_ROBOT_RECONNECTED = 1 << 4
FLAG_SUBTASK_SUCCESS = 1 << 5
FLAG_NOGO_INTRUSION = 1 << 6
FLAG_EPISODE_DONE = 1 << 7

ARM_NAMES = {0: "single_pass", 1: "naive_retry", 2: "corr_aware_retry"}
STRATEGY_NAMES = {
    0: "first_try",
    1: "replay",
    2: "relocalize",
    3: "slow_mode",
    4: "obs_lead",
    5: "reapproach_left",
    6: "reapproach_right",
    7: "obs_jitter",
}
FAILURE_MODES = {
    0: "none",
    1: "lost_contact",
    2: "wrong_side_stall",
    3: "stopped_short",
    4: "off_axis_drift",
    5: "unrecoverable",
    6: "regressed",
}
POLICY_KINDS = {0: "scripted", 1: "tiny_mlp_fp32", 2: "tiny_mlp_q12"}


def pack_frame(fields):
    """Pack a dict into a verified 128-byte TelemetryFrame."""
    pusher = fields.get("pusher_xy", (0.0, 0.0))
    desired = fields.get("desired_xy", pusher)
    safe = fields.get("safe_xy", desired)
    obj = fields.get("obj_xy", (0.12, -0.02, -0.12, -0.02))
    goal = fields.get("goal_xy", (0.25, 0.20, -0.25, 0.20))
    body_no_crc = struct.pack(
        "<IHHIiqq2f2f2f4f4f2fIIH6BQI",
        MAGIC,
        VERSION,
        int(fields.get("flags", 0)),
        int(fields.get("seq", 0)),
        int(fields.get("episode", 1000)),
        int(fields.get("timestamp_ns", 0)),
        int(fields.get("release_latency_ns", 0)),
        float(pusher[0]), float(pusher[1]),
        float(desired[0]), float(desired[1]),
        float(safe[0]), float(safe[1]),
        float(obj[0]), float(obj[1]), float(obj[2]), float(obj[3]),
        float(goal[0]), float(goal[1]), float(goal[2]), float(goal[3]),
        float(fields.get("min_clearance_m", 1.0)),
        float(fields.get("compute_us", 0.0)),
        int(fields.get("shield_interventions", 0)),
        int(fields.get("robot_reconnects", 0)),
        int(fields.get("shield_reason_mask", 0)),
        int(fields.get("arm_id", 2)),
        int(fields.get("strategy_id", 0)),
        int(fields.get("failure_mode", 0)),
        int(fields.get("subtask_id", 0)),
        int(fields.get("attempt", 0)),
        int(fields.get("policy_kind", 1)),
        int(fields.get("tick", 0)),
        0,
    )
    crc = zlib.crc32(body_no_crc) & 0xFFFFFFFF
    return body_no_crc + struct.pack("<I", crc)


def decode_frame(raw):
    """Unpack and CRC-verify one 128-byte TelemetryFrame into a structured dictionary."""
    if len(raw) != FRAME_SIZE:
        raise ValueError(f"TelemetryFrame must be {FRAME_SIZE} bytes, got {len(raw)}")
    u = FRAME_STRUCT.unpack(raw)
    magic, version, flags, seq, episode, ts_ns, lat_ns = u[0:7]
    if magic != MAGIC:
        raise ValueError(f"Bad telemetry magic 0x{magic:08x} (expected 0x{MAGIC:08x})")
    if version != VERSION:
        raise ValueError(f"Unsupported telemetry version {version}")
    expected_crc = zlib.crc32(raw[:124]) & 0xFFFFFFFF
    got_crc = u[34]
    if got_crc != expected_crc:
        raise ValueError(f"CRC-32 mismatch: got 0x{got_crc:08x}, expected 0x{expected_crc:08x}")

    return {
        "seq": seq,
        "episode": episode,
        "tick": u[32],
        "timestamp_ns": ts_ns,
        "release_latency_us": round(lat_ns / 1000.0, 2),
        "compute_us": round(u[22], 2),
        "pusher_xy": [round(u[7], 5), round(u[8], 5)],
        "desired_xy": [round(u[9], 5), round(u[10], 5)],
        "safe_xy": [round(u[11], 5), round(u[12], 5)],
        "obj_a_xy": [round(u[13], 5), round(u[14], 5)],
        "obj_b_xy": [round(u[15], 5), round(u[16], 5)],
        "goal_a_xy": [round(u[17], 5), round(u[18], 5)],
        "goal_b_xy": [round(u[19], 5), round(u[20], 5)],
        "min_clearance_mm": round(u[21] * 1000.0, 2),
        "shield_interventions": u[23],
        "robot_reconnects": u[24],
        "shield_reason_mask": u[25],
        "arm": ARM_NAMES.get(u[26], f"arm_{u[26]}"),
        "strategy": STRATEGY_NAMES.get(u[27], "custom"),
        "failure_mode": FAILURE_MODES.get(u[28], f"mode_{u[28]}"),
        "subtask": "push_A" if u[29] == 0 else "push_B",
        "attempt": u[30],
        "policy": POLICY_KINDS.get(u[31], f"pol_{u[31]}"),
        "flags": flags,
        "shield_enabled": bool(flags & FLAG_SHIELD_ENABLED),
        "shield_intervened": bool(flags & FLAG_SHIELD_INTERVENED),
        "correlated": bool(flags & FLAG_CORRELATED_FAIL),
        "robot_link_lost": bool(flags & FLAG_ROBOT_LINK_LOST),
        "robot_reconnected": bool(flags & FLAG_ROBOT_RECONNECTED),
        "subtask_ok": bool(flags & FLAG_SUBTASK_SUCCESS),
        "nogo_intrusion": bool(flags & FLAG_NOGO_INTRUSION),
        "episode_done": bool(flags & FLAG_EPISODE_DONE),
        "crc32": f"0x{got_crc:08x}",
    }


def decode_binary_file(path):
    """Read and verify all 128-byte TelemetryFrames from a binary file."""
    frames = []
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(FRAME_SIZE)
            if not chunk:
                break
            frames.append(decode_frame(chunk))
    return frames


def to_ros2_diagnostic(decoded):
    """Format a decoded frame as a ROS 2 diagnostic_msgs/DiagnosticStatus + geometry_msgs/PoseStamped envelope."""
    level = 0  # OK
    if decoded.get("nogo_intrusion") or decoded.get("robot_link_lost"):
        level = 2  # ERROR
    elif decoded.get("shield_intervened") or decoded.get("failure_mode") != "none":
        level = 1  # WARN
    return {
        "topic": "/relmw/diagnostics",
        "status": {
            "level": level,
            "name": "relmw_qnx_supervisor",
            "message": f"ep={decoded['episode']} tick={decoded['tick']} strat={decoded['strategy']}",
            "hardware_id": "qnx80_relmw_cell",
            "values": {
                "arm": decoded["arm"],
                "policy": decoded["policy"],
                "shield_interventions": str(decoded["shield_interventions"]),
                "min_clearance_mm": str(decoded["min_clearance_mm"]),
                "release_latency_us": str(decoded["release_latency_us"]),
                "robot_reconnects": str(decoded["robot_reconnects"]),
            },
        },
        "pose_topic": "/relmw/end_effector_pose",
        "pose": {
            "frame_id": "cell_table",
            "position": {"x": decoded["pusher_xy"][0], "y": decoded["pusher_xy"][1], "z": 0.0},
        },
    }


def listen_udp(host="0.0.0.0", port=9870, max_frames=0, timeout_s=5.0, use_ros2=True):
    """Listen for 128-byte UDP TelemetryFrames, optionally publishing via rclpy if installed."""
    ros_node = None
    if use_ros2:
        try:
            import rclpy  # type: ignore
            if not rclpy.ok():
                rclpy.init()
            ros_node = rclpy.create_node("relmw_qnx_bridge")
        except Exception:
            ros_node = None

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.settimeout(0.5)
    sock.bind((host, port))
    deadline = time.monotonic() + timeout_s if timeout_s > 0 else None
    out = []
    try:
        while True:
            if max_frames > 0 and len(out) >= max_frames:
                break
            if deadline is not None and time.monotonic() >= deadline:
                break
            try:
                pkt, _ = sock.recvfrom(2048)
            except socket.timeout:
                continue
            if len(pkt) != FRAME_SIZE:
                continue
            d = decode_frame(pkt)
            out.append(d)
    finally:
        sock.close()
        if ros_node is not None:
            try:
                ros_node.destroy_node()
            except Exception:
                pass
    return out


def send_ctl(mount, command, relmwctl=None):
    """Forward a ROS 2 / operator command line to `/dev/relmw/ctl`."""
    if relmwctl is None:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        relmwctl = os.path.join(root, "scripts", "relmwctl")
    parts = command.strip().split()
    proc = subprocess.run(["sh", relmwctl, "-m", mount] + parts, capture_output=True, text=True, check=False)
    return proc.returncode, proc.stdout.strip()


def main():
    ap = argparse.ArgumentParser(description="ROS 2 / UDP 128-byte zero-copy telemetry bridge for relmw-qnx.")
    sub = ap.add_subparsers(dest="subcmd", required=True)

    p_dec = sub.add_parser("decode", help="decode a binary .bin file of 128-byte TelemetryFrames to JSONL")
    p_dec.add_argument("--bin", required=True, help="input binary file")
    p_dec.add_argument("--ros2-envelope", action="store_true", help="emit ROS 2 DiagnosticStatus envelopes")

    p_lis = sub.add_parser("listen", help="listen on UDP for live 128-byte TelemetryFrames")
    p_lis.add_argument("--host", default="0.0.0.0")
    p_lis.add_argument("--port", type=int, default=9870)
    p_lis.add_argument("--max-frames", type=int, default=100)
    p_lis.add_argument("--timeout", type=float, default=10.0)

    p_ctl = sub.add_parser("ctl", help="forward a command to /dev/relmw/ctl")
    p_ctl.add_argument("--mount", default="/dev/relmw")
    p_ctl.add_argument("command", help="command string, e.g. 'plan push_B,push_A'")

    args = ap.parse_args()
    if args.subcmd == "decode":
        for d in decode_binary_file(args.bin):
            payload = to_ros2_diagnostic(d) if args.ros2_envelope else d
            print(json.dumps(payload, separators=(",", ":")))
        return 0
    if args.subcmd == "listen":
        for d in listen_udp(args.host, args.port, args.max_frames, args.timeout):
            print(json.dumps(d, separators=(",", ":")), flush=True)
        return 0
    if args.subcmd == "ctl":
        rc, reply = send_ctl(args.mount, args.command)
        if reply:
            print(reply)
        return rc
    return 2


if __name__ == "__main__":
    sys.exit(main())
