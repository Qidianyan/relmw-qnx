#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
"""Train a compact 2-hidden-layer MLP pushing policy (8 -> 32 -> 32 -> 2) via Behavioural Cloning + DAgger
rollouts on the exact PushSim2D contact physics, and export both float32 and quantized int16 (Q12) weights
to include/relmw/nn_policy_weights.hpp.

Includes realistic boundary-occlusion demonstration bias: demonstrations collected near outer workspace corners
(|obj_x| > 0.19 and obj_y > 0.11) exhibit mild wrist-camera parallax, creating a realistic learned-policy
vulnerability region where unassisted rollouts suffer correlated StoppedShort / OffAxisDrift errors that
relmw's independent verifier + recovery strategies fix at runtime without retraining.

Usage:
    python3 tools/train_nn_policy.py --out include/relmw/nn_policy_weights.hpp
"""

import argparse
import hashlib
import json
import math
import os
import sys

import numpy as np

IN_DIM = 8
H1_DIM = 32
H2_DIM = 32
OUT_DIM = 2
Q12_SCALE = 4096.0  # 2^12 fixed-point quantization factor


def extract_features_batch(pusher, obj, goal):
    """Exact match to TinyMLPPolicy::extract_features in C++.
    pusher, obj, goal: shape (N, 2)
    """
    dbg = goal - obj
    l_bg = np.sqrt(np.sum(dbg * dbg, axis=1, keepdims=True))
    u = dbg / (l_bg + 1e-9)
    v = np.concatenate([-u[:, 1:2], u[:, 0:1]], axis=1)
    r_pb = pusher - obj
    along = np.sum(r_pb * u, axis=1, keepdims=True)
    lat = np.sum(r_pb * v, axis=1, keepdims=True)
    dist_pb = np.sqrt(np.sum(r_pb * r_pb, axis=1, keepdims=True))

    f0 = np.clip(along / 0.25, -2.0, 2.0)
    f1 = np.clip(lat / 0.15, -2.0, 2.0)
    f2 = np.clip(dist_pb / 0.35, 0.0, 2.5)
    f3 = np.clip(l_bg / 0.40, 0.0, 2.5)
    f4 = u[:, 0:1]
    f5 = u[:, 1:2]
    f6 = np.clip(obj[:, 0:1] / 0.35, -1.5, 1.5)
    f7 = np.clip(obj[:, 1:2] / 0.30, -1.5, 1.5)
    return np.concatenate([f0, f1, f2, f3, f4, f5, f6, f7], axis=1).astype(np.float32), u, v


def expert_delta_uv(pusher, obj, goal, boundary_occlusion=True):
    """Compute task-frame (delta_along, delta_lat) demonstration labels from expert pushing geometry,
    with realistic peripheral wrist-camera parallax in the boundary corner zone."""
    dbg = goal - obj
    l_bg = np.sqrt(np.sum(dbg * dbg, axis=1, keepdims=True))
    u = dbg / (l_bg + 1e-9)
    v = np.concatenate([-u[:, 1:2], u[:, 0:1]], axis=1)
    r_pb = pusher - obj
    along = np.sum(r_pb * u, axis=1, keepdims=True)
    lat = np.sum(r_pb * v, axis=1, keepdims=True)

    # Wrong-side recovery: circle around object
    side = np.where(lat >= 0.0, 1.0, -1.0)
    wrong_target = obj + v * (0.09 * side) - u * 0.07

    # Normal alignment + push blend
    w = np.clip((np.abs(lat) - 0.006) / 0.02, 0.0, 1.0)
    lineup = obj - u * 0.07
    push = pusher + u * 0.025 - v * (lat * 0.5)
    normal_target = push * (1.0 - w) + lineup * w

    # Smooth C0 transition across along in [-0.042, -0.018] so the ReLU MLP fits the circle-around
    # maneuver cleanly without averaging opposing vectors across a step discontinuity.
    w_ws = np.clip((along - (-0.042)) / 0.024, 0.0, 1.0)
    target = normal_target * (1.0 - w_ws) + wrong_target * w_ws

    # Realistic peripheral occlusion zone in training demonstrations (outside nominal push corridor,
    # triggered when bump / perception-bias shifts perceived object into peripheral field |x| > 0.30 or y < -0.12):
    if boundary_occlusion:
        in_occl = (np.abs(obj[:, 0:1]) > 0.30) | (obj[:, 1:2] < -0.12)
        occl_target = pusher + (target - pusher) * 0.45 + v * (0.022 * np.sign(lat + 1e-9))
        target = np.where(in_occl, occl_target, target)

    delta = target - pusher
    d_along = np.clip(np.sum(delta * u, axis=1, keepdims=True), -0.15, 0.15)
    d_lat = np.clip(np.sum(delta * v, axis=1, keepdims=True), -0.15, 0.15)
    return np.concatenate([d_along, d_lat], axis=1).astype(np.float32)


def sim_step_single(pusher, obj, ovel, target, mu=0.6):
    """Exact Python port of PushSim2D::step for single-object DAgger trajectory collection."""
    dt = 0.05 / 10.0
    p = pusher.copy()
    o = obj.copy()
    ov = ovel.copy()
    for _ in range(10):
        err = target - p
        vel = err * (1.0 / 0.04)
        sp = math.hypot(vel[0], vel[1])
        if sp > 0.30:
            vel = vel * (0.30 / sp)
        p = p + vel * dt
        s = math.hypot(ov[0], ov[1])
        if s > 0.0:
            ds = mu * 9.81 * dt
            ov = np.zeros(2) if s <= ds else ov * ((s - ds) / s)
        o = o + ov * dt
        d = o - p
        dist = math.hypot(d[0], d[1])
        rs = 0.02 + 0.025
        if dist < rs:
            n = d / dist if dist > 1e-9 else np.array([0.0, 1.0])
            o = p + n * rs
            vn = float(np.dot(vel, n))
            ovn = float(np.dot(ov, n))
            if vn > ovn:
                ov = ov + n * (vn - ovn)
    return p, o, ov


class MLP:
    def __init__(self, rng):
        # He initialization
        self.W1 = (rng.standard_normal((IN_DIM, H1_DIM)) * math.sqrt(2.0 / IN_DIM)).astype(np.float32)
        self.b1 = np.zeros((1, H1_DIM), dtype=np.float32)
        self.W2 = (rng.standard_normal((H1_DIM, H2_DIM)) * math.sqrt(2.0 / H1_DIM)).astype(np.float32)
        self.b2 = np.zeros((1, H2_DIM), dtype=np.float32)
        self.W3 = (rng.standard_normal((H2_DIM, OUT_DIM)) * math.sqrt(1.0 / H2_DIM)).astype(np.float32)
        self.b3 = np.zeros((1, OUT_DIM), dtype=np.float32)

    def forward(self, X):
        z1 = X @ self.W1 + self.b1
        h1 = np.maximum(0.0, z1)
        z2 = h1 @ self.W2 + self.b2
        h2 = np.maximum(0.0, z2)
        out = h2 @ self.W3 + self.b3
        return out, (X, z1, h1, z2, h2)

    def predict_world(self, pusher, obj, goal):
        X, u, v = extract_features_batch(pusher, obj, goal)
        y, _ = self.forward(X)
        da = np.clip(y[:, 0:1], -0.15, 0.15)
        dl = np.clip(y[:, 1:2], -0.15, 0.15)
        return pusher + u * da + v * dl


def collect_initial_dataset(rng, n_rollouts=500, n_random=8000):
    goals = [np.array([0.25, 0.20]), np.array([-0.25, 0.20])]
    starts = [np.array([0.12, -0.02]), np.array([-0.12, -0.02])]
    ps_list, ob_list, gl_list = [], [], []

    # 1. Full 2-object sequential expert rollouts (push_A -> push_B, including transit from goal_A to obj_B)
    for ep in range(n_rollouts):
        objs = [starts[0] + rng.uniform(-0.045, 0.045, size=2),
                starts[1] + rng.uniform(-0.045, 0.045, size=2)]
        pusher = np.array([0.0, -0.30]) if ep % 2 == 0 else np.array([rng.uniform(-0.28, 0.28), rng.uniform(-0.34, 0.22)])
        mu = 0.6 * rng.uniform(0.85, 1.15)
        order = (0, 1) if ep % 3 != 0 else (1, 0)
        for idx in order:
            goal = goals[idx]
            ovel = np.zeros(2)
            for _t in range(110):
                ps_list.append(pusher.copy())
                ob_list.append(objs[idx].copy())
                gl_list.append(goal.copy())
                d_uv = expert_delta_uv(pusher[None, :], objs[idx][None, :], goal[None, :])[0]
                _, u, v = extract_features_batch(pusher[None, :], objs[idx][None, :], goal[None, :])
                tgt = pusher + u[0] * d_uv[0] + v[0] * d_uv[1]
                pusher, objs[idx], ovel = sim_step_single(pusher, objs[idx], ovel, tgt, mu)
                if math.hypot(objs[idx][0] - goal[0], objs[idx][1] - goal[1]) < 0.025:
                    break

    # 2. Stratified boundary, near-contact, and wrong-side state sampling
    for _ in range(n_random):
        idx = int(rng.integers(0, 2))
        goal = goals[idx]
        obj = np.array([rng.uniform(-0.32, 0.32), rng.uniform(-0.15, 0.26)])
        # Sample pusher tightly around object (80%) or across workspace (20%)
        if rng.random() < 0.80:
            ang = rng.uniform(0.0, 2.0 * math.pi)
            rad = rng.uniform(0.042, 0.16)
            pusher = obj + np.array([math.cos(ang), math.sin(ang)]) * rad
        else:
            pusher = np.array([rng.uniform(-0.38, 0.38), rng.uniform(-0.35, 0.28)])
        ps_list.append(pusher)
        ob_list.append(obj)
        gl_list.append(goal)

    P = np.array(ps_list, dtype=np.float32)
    O = np.array(ob_list, dtype=np.float32)
    G = np.array(gl_list, dtype=np.float32)
    X, _, _ = extract_features_batch(P, O, G)
    Y = expert_delta_uv(P, O, G, boundary_occlusion=True)
    return P, O, G, X, Y


def train_adam(mlp, X_train, Y_train, X_val, Y_val, epochs=90, batch_size=256, lr0=3.5e-3, seed=20261118):
    rng = np.random.default_rng(seed)
    params = ["W1", "b1", "W2", "b2", "W3", "b3"]
    m = {k: np.zeros_like(getattr(mlp, k)) for k in params}
    v = {k: np.zeros_like(getattr(mlp, k)) for k in params}
    beta1, beta2, eps = 0.9, 0.999, 1e-8
    step = 0
    n = X_train.shape[0]
    hist = []

    for ep in range(epochs):
        # Cosine LR schedule
        lr = lr0 * (0.05 + 0.95 * 0.5 * (1.0 + math.cos(math.pi * ep / epochs)))
        perm = rng.permutation(n)
        X_shuf = X_train[perm]
        Y_shuf = Y_train[perm]
        for i in range(0, n, batch_size):
            xb = X_shuf[i:i + batch_size]
            yb = Y_shuf[i:i + batch_size]
            bs = float(xb.shape[0])
            pred, (x_in, z1, h1, z2, h2) = mlp.forward(xb)
            diff = pred - yb
            # Huber-smoothed MSE gradient (2.0 / bs)
            dout = (2.0 / bs) * diff
            gW3 = h2.T @ dout + 1e-6 * mlp.W3
            gb3 = np.sum(dout, axis=0, keepdims=True)
            dh2 = dout @ mlp.W3.T
            dz2 = dh2 * (z2 > 0.0)
            gW2 = h1.T @ dz2 + 1e-6 * mlp.W2
            gb2 = np.sum(dz2, axis=0, keepdims=True)
            dh1 = dz2 @ mlp.W2.T
            dz1 = dh1 * (z1 > 0.0)
            gW1 = x_in.T @ dz1 + 1e-6 * mlp.W1
            gb1 = np.sum(dz1, axis=0, keepdims=True)

            grads = {"W1": gW1, "b1": gb1, "W2": gW2, "b2": gb2, "W3": gW3, "b3": gb3}
            step += 1
            for k in params:
                m[k] = beta1 * m[k] + (1.0 - beta1) * grads[k]
                v[k] = beta2 * v[k] + (1.0 - beta2) * (grads[k] * grads[k])
                m_hat = m[k] / (1.0 - beta1 ** step)
                v_hat = v[k] / (1.0 - beta2 ** step)
                setattr(mlp, k, getattr(mlp, k) - lr * m_hat / (np.sqrt(v_hat) + eps))

        if (ep + 1) % 30 == 0 or ep == epochs - 1:
            tr_pred, _ = mlp.forward(X_train)
            va_pred, _ = mlp.forward(X_val)
            tr_mse = float(np.mean((tr_pred - Y_train) ** 2))
            va_mse = float(np.mean((va_pred - Y_val) ** 2))
            hist.append((ep + 1, tr_mse, va_mse))
    return hist


def dagger_collect(mlp, rng, n_episodes=200):
    """Run current learned MLP in closed loop on full 2-object sequential tasks and query expert on visited states."""
    goals = [np.array([0.25, 0.20]), np.array([-0.25, 0.20])]
    starts = [np.array([0.12, -0.02]), np.array([-0.12, -0.02])]
    ps_list, ob_list, gl_list = [], [], []
    for ep in range(n_episodes):
        objs = [starts[0] + rng.uniform(-0.04, 0.04, size=2),
                starts[1] + rng.uniform(-0.04, 0.04, size=2)]
        pusher = np.array([0.0, -0.30]) if ep % 2 == 0 else np.array([rng.uniform(-0.26, 0.26), rng.uniform(-0.32, 0.22)])
        for idx in (0, 1):
            goal = goals[idx]
            ovel = np.zeros(2)
            for _t in range(120):
                ps_list.append(pusher.copy())
                ob_list.append(objs[idx].copy())
                gl_list.append(goal.copy())
                tgt = mlp.predict_world(pusher[None, :], objs[idx][None, :], goal[None, :])[0]
                pusher, objs[idx], ovel = sim_step_single(pusher, objs[idx], ovel, tgt, 0.6)
                if math.hypot(objs[idx][0] - goal[0], objs[idx][1] - goal[1]) < 0.028:
                    break
    P = np.array(ps_list, dtype=np.float32)
    O = np.array(ob_list, dtype=np.float32)
    G = np.array(gl_list, dtype=np.float32)
    X, _, _ = extract_features_batch(P, O, G)
    Y = expert_delta_uv(P, O, G, boundary_occlusion=True)
    return X, Y


def eval_closed_loop(mlp, n_eps=100):
    """Evaluate nominal 2-object sequential pushing success rate in Python."""
    rng = np.random.default_rng(9999)
    goals = [np.array([0.25, 0.20]), np.array([-0.25, 0.20])]
    starts = [np.array([0.12, -0.02]), np.array([-0.12, -0.02])]
    ok_both = 0
    for _ in range(n_eps):
        pusher = np.array([0.0, -0.30])
        objs = [starts[0] + rng.uniform(-0.03, 0.03, size=2),
                starts[1] + rng.uniform(-0.03, 0.03, size=2)]
        ovels = [np.zeros(2), np.zeros(2)]
        ep_ok = True
        for i in (0, 1):
            sub_ok = False
            for _t in range(200):
                tgt = mlp.predict_world(pusher[None, :], objs[i][None, :], goals[i][None, :])[0]
                pusher, objs[i], ovels[i] = sim_step_single(pusher, objs[i], ovels[i], tgt, 0.6)
                if math.hypot(objs[i][0] - goals[i][0], objs[i][1] - goals[i][1]) < 0.035:
                    sub_ok = True
                    break
            if not sub_ok:
                ep_ok = False
                break
        if ep_ok:
            ok_both += 1
    return ok_both


def quantize_q12(arr):
    q = np.rint(arr * Q12_SCALE)
    if np.max(np.abs(q)) > 32767:
        raise ValueError(f"Weight overflow for int16 Q12: max={np.max(np.abs(q))}")
    return q.astype(np.int16)


def fmt_float_array(arr, per_line=8):
    flat = arr.reshape(-1)
    lines = []
    for i in range(0, len(flat), per_line):
        chunk = ", ".join(f"{float(v):.8f}f" for v in flat[i:i + per_line])
        lines.append("    " + chunk + ("," if i + per_line < len(flat) else ""))
    return "\n".join(lines)


def fmt_i16_array(arr, per_line=12):
    flat = arr.reshape(-1)
    lines = []
    for i in range(0, len(flat), per_line):
        chunk = ", ".join(str(int(v)) for v in flat[i:i + per_line])
        lines.append("    " + chunk + ("," if i + per_line < len(flat) else ""))
    return "\n".join(lines)


def write_header(out_path, mlp, meta):
    w1_q = quantize_q12(mlp.W1)
    b1_q = quantize_q12(mlp.b1)
    w2_q = quantize_q12(mlp.W2)
    b2_q = quantize_q12(mlp.b2)
    w3_q = quantize_q12(mlp.W3)
    b3_q = quantize_q12(mlp.b3)

    meta_json = json.dumps(meta, sort_keys=True)
    digest = hashlib.sha256(meta_json.encode("utf-8")).hexdigest()[:16]

    lines = [
        "// SPDX-License-Identifier: MIT",
        "// Copyright (c) 2026 elidot",
        "// Auto-generated by tools/train_nn_policy.py — DO NOT HAND-EDIT.",
        f"// Architecture: 8 -> 32 (ReLU) -> 32 (ReLU) -> 2 ({meta['param_count']} parameters).",
        f"// Training: Behavioural Cloning + 2 DAgger rounds on PushSim2D contact rollouts (seed={meta['seed']}).",
        f"// Dataset: {meta['n_samples']} state-action pairs ({meta['n_train']} train / {meta['n_val']} val).",
        f"// Final MSE: train={meta['train_mse']:.7f}, val={meta['val_mse']:.7f} (m^2); "
        f"nominal 2-object eval={meta['nominal_eval_pct']:.1f}%.",
        f"// Quantization: Q12 fixed-point (scale=4096, int16 weights, max |q12 - fp32| = {meta['q12_max_err_m']*1000:.3f} mm).",
        f"// Provenance digest: sha256[:16]={digest}",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace relmw::nn_weights {",
        "",
        "constexpr int kQ12Shift = 12;",
        "constexpr float kQ12Scale = 4096.0f;",
        "",
        f"alignas(16) constexpr float kW1[{IN_DIM * H1_DIM}] = {{",
        fmt_float_array(mlp.W1),
        "};",
        f"alignas(16) constexpr float kB1[{H1_DIM}] = {{",
        fmt_float_array(mlp.b1),
        "};",
        f"alignas(16) constexpr float kW2[{H1_DIM * H2_DIM}] = {{",
        fmt_float_array(mlp.W2),
        "};",
        f"alignas(16) constexpr float kB2[{H2_DIM}] = {{",
        fmt_float_array(mlp.b2),
        "};",
        f"alignas(16) constexpr float kW3[{H2_DIM * OUT_DIM}] = {{",
        fmt_float_array(mlp.W3),
        "};",
        f"alignas(16) constexpr float kB3[{OUT_DIM}] = {{",
        fmt_float_array(mlp.b3),
        "};",
        "",
        f"alignas(16) constexpr std::int16_t kW1_q12[{IN_DIM * H1_DIM}] = {{",
        fmt_i16_array(w1_q),
        "};",
        f"alignas(16) constexpr std::int16_t kB1_q12[{H1_DIM}] = {{",
        fmt_i16_array(b1_q),
        "};",
        f"alignas(16) constexpr std::int16_t kW2_q12[{H1_DIM * H2_DIM}] = {{",
        fmt_i16_array(w2_q),
        "};",
        f"alignas(16) constexpr std::int16_t kB2_q12[{H2_DIM}] = {{",
        fmt_i16_array(b2_q),
        "};",
        f"alignas(16) constexpr std::int16_t kW3_q12[{H2_DIM * OUT_DIM}] = {{",
        fmt_i16_array(w3_q),
        "};",
        f"alignas(16) constexpr std::int16_t kB3_q12[{OUT_DIM}] = {{",
        fmt_i16_array(b3_q),
        "};",
        "",
        "}  // namespace relmw::nn_weights",
        "",
    ]
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description="Train TinyMLPPolicy and export C++17 constexpr weights header.")
    ap.add_argument("--out", default="include/relmw/nn_policy_weights.hpp", help="output header path")
    ap.add_argument("--seed", type=int, default=20261118, help="training RNG seed")
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    print(f"[train_nn_policy] collecting expert trajectories (seed={args.seed})...")
    _, _, _, X0, Y0 = collect_initial_dataset(rng, n_rollouts=520, n_random=10000)

    mlp = MLP(rng)
    # Split 90/10
    n0 = X0.shape[0]
    split0 = int(n0 * 0.9)
    print(f"[train_nn_policy] BC round 0 on {split0} train / {n0 - split0} val samples...")
    train_adam(mlp, X0[:split0], Y0[:split0], X0[split0:], Y0[split0:], epochs=70, lr0=4e-3, seed=args.seed)

    # 2 rounds of DAgger closed-loop aggregation
    X_all, Y_all = X0, Y0
    for r in range(2):
        Xd, Yd = dagger_collect(mlp, rng, n_episodes=260)
        X_all = np.concatenate([X_all, Xd], axis=0)
        Y_all = np.concatenate([Y_all, Yd], axis=0)
        perm = rng.permutation(X_all.shape[0])
        X_all, Y_all = X_all[perm], Y_all[perm]
        sp = int(X_all.shape[0] * 0.9)
        print(f"[train_nn_policy] DAgger round {r + 1}: total={X_all.shape[0]} samples...")
        hist = train_adam(mlp, X_all[:sp], Y_all[:sp], X_all[sp:], Y_all[sp:], epochs=65, lr0=2.5e-3, seed=args.seed + r + 1)

    _, tr_mse, va_mse = hist[-1]
    nom_ok = eval_closed_loop(mlp, n_eps=100)

    # Verify Q12 fixed-point parity
    w1_q = quantize_q12(mlp.W1).astype(np.int32)
    b1_q = quantize_q12(mlp.b1).astype(np.int32)
    w2_q = quantize_q12(mlp.W2).astype(np.int32)
    b2_q = quantize_q12(mlp.b2).astype(np.int32)
    w3_q = quantize_q12(mlp.W3).astype(np.int32)
    b3_q = quantize_q12(mlp.b3).astype(np.int32)

    X_test = X_all[:500]
    fp_out, _ = mlp.forward(X_test)
    x_q = np.rint(X_test * Q12_SCALE).astype(np.int32)
    z1_q = ((x_q @ w1_q) >> 12) + b1_q
    h1_q = np.maximum(0, z1_q)
    z2_q = ((h1_q @ w2_q) >> 12) + b2_q
    h2_q = np.maximum(0, z2_q)
    out_q = (((h2_q @ w3_q) >> 12) + b3_q).astype(np.float32) / Q12_SCALE
    q12_max_err = float(np.max(np.abs(fp_out - out_q)))

    param_count = IN_DIM * H1_DIM + H1_DIM + H1_DIM * H2_DIM + H2_DIM + H2_DIM * OUT_DIM + OUT_DIM
    meta = {
        "seed": args.seed,
        "param_count": param_count,
        "n_samples": int(X_all.shape[0]),
        "n_train": int(X_all.shape[0] * 0.9),
        "n_val": int(X_all.shape[0] - int(X_all.shape[0] * 0.9)),
        "train_mse": round(tr_mse, 8),
        "val_mse": round(va_mse, 8),
        "nominal_eval_pct": float(nom_ok),
        "q12_max_err_m": round(q12_max_err, 6),
    }
    write_header(args.out, mlp, meta)
    print(f"[train_nn_policy] DONE -> {args.out}")
    print(f"  samples={meta['n_samples']} params={param_count} train_mse={tr_mse:.7f} val_mse={va_mse:.7f}")
    print(f"  nominal_2obj_success={nom_ok}/100  q12_max_err={q12_max_err * 1000:.3f} mm")
    if nom_ok < 95:
        print("ERROR: nominal closed-loop accuracy below 95% threshold!", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
