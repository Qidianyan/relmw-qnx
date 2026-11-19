// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
#include "relmw/safety_shield.hpp"
#include <algorithm>
#include <cmath>

namespace relmw {

namespace {

// Closest point on segment [a, b] to query point p. O(1), stack only.
inline Vec2 closest_on_segment(Vec2 a, Vec2 b, Vec2 p) noexcept {
  const Vec2 ab = b - a;
  const double len2 = ab.dot(ab);
  if (len2 <= 1e-18) return a;
  const double t = std::clamp((p - a).dot(ab) / len2, 0.0, 1.0);
  return a + ab * t;
}

// Minimum distance squared between segments [a, b] and [c, d]. O(1), stack only.
inline double segment_segment_dist(Vec2 a, Vec2 b, Vec2 c, Vec2 d) noexcept {
  auto orient = [](Vec2 p, Vec2 q, Vec2 r) noexcept {
    return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
  };
  auto on_seg = [](Vec2 p, Vec2 q, Vec2 r) noexcept {
    return q.x >= std::min(p.x, r.x) - 1e-12 && q.x <= std::max(p.x, r.x) + 1e-12 &&
           q.y >= std::min(p.y, r.y) - 1e-12 && q.y <= std::max(p.y, r.y) + 1e-12;
  };
  const double o1 = orient(a, b, c), o2 = orient(a, b, d);
  const double o3 = orient(c, d, a), o4 = orient(c, d, b);
  if (((o1 > 0 && o2 < 0) || (o1 < 0 && o2 > 0)) && ((o3 > 0 && o4 < 0) || (o3 < 0 && o4 > 0))) return 0.0;
  if (std::fabs(o1) <= 1e-12 && on_seg(a, c, b)) return 0.0;
  if (std::fabs(o2) <= 1e-12 && on_seg(a, d, b)) return 0.0;
  if (std::fabs(o3) <= 1e-12 && on_seg(c, a, d)) return 0.0;
  if (std::fabs(o4) <= 1e-12 && on_seg(c, b, d)) return 0.0;
  const double d1 = (closest_on_segment(a, b, c) - c).norm();
  const double d2 = (closest_on_segment(a, b, d) - d).norm();
  const double d3 = (closest_on_segment(c, d, a) - a).norm();
  const double d4 = (closest_on_segment(c, d, b) - b).norm();
  return std::min(std::min(d1, d2), std::min(d3, d4));
}

// Signed distance from point p to CCW convex polygon (negative inside, >=0 outside), plus closest boundary point
// and outward unit normal. O(n_verts), zero heap allocation.
struct PolyContact {
  double signed_dist = 1e9;
  Vec2 boundary_pt{0.0, 0.0};
  Vec2 outward_normal{0.0, 1.0};
};

inline PolyContact query_convex_polygon(const ConvexPolygonZone& poly, Vec2 p) noexcept {
  PolyContact res;
  const int n = poly.n_verts;
  if (!poly.active || n < 3) return res;

  bool inside = true;
  double best_ext_dist = 1e9;
  Vec2 best_ext_pt = poly.verts[0];
  Vec2 best_ext_n{0.0, 1.0};

  double best_int_depth = -1e9;   // max (least negative) signed plane distance <= 0
  Vec2 best_int_pt = poly.verts[0];
  Vec2 best_int_n{0.0, 1.0};

  for (int i = 0; i < n; ++i) {
    const Vec2 a = poly.verts[i];
    const Vec2 b = poly.verts[(i + 1) % n];
    const Vec2 edge = b - a;
    const double elen = edge.norm();
    if (elen <= 1e-12) continue;
    // CCW polygon: edge.perp() is (-ey, ex) = inward normal; outward normal is (ey, -ex) / elen.
    const Vec2 out_n{edge.y / elen, -edge.x / elen};
    const double plane_d = (p - a).dot(out_n);
    if (plane_d > 0.0) inside = false;
    if (plane_d > best_int_depth) {
      best_int_depth = plane_d;
      best_int_n = out_n;
      best_int_pt = p - out_n * plane_d;
    }
    const Vec2 q = closest_on_segment(a, b, p);
    const Vec2 diff = p - q;
    const double d = diff.norm();
    if (d < best_ext_dist) {
      best_ext_dist = d;
      best_ext_pt = q;
      best_ext_n = d > 1e-9 ? diff * (1.0 / d) : out_n;
    }
  }

  if (inside) {
    res.signed_dist = best_int_depth;  // <= 0
    res.boundary_pt = best_int_pt;
    res.outward_normal = best_int_n;
  } else {
    res.signed_dist = best_ext_dist;   // >= 0
    res.boundary_pt = best_ext_pt;
    res.outward_normal = best_ext_n;
  }
  return res;
}

}  // namespace

SafetyEnvelope SafetyEnvelope::industrial_default() {
  SafetyEnvelope e;
  e.workspace_half_x = kTableX;
  e.workspace_half_y = kTableY;
  e.tool_radius = kPusherR;
  e.max_step_delta = 0.055;
  e.max_step_accel = 0.040;
  e.cbf_alpha = 0.50;
  e.n_circles = 0;
  e.n_polygons = 0;
  return e;
}

SafetyEnvelope SafetyEnvelope::medical_surgical_fixture() {
  SafetyEnvelope e = industrial_default();
  e.max_step_delta = 0.085;
  e.max_step_accel = 0.065;
  e.cbf_alpha = 0.55;

  // Circle 0: Vascular / Fragile Sterile Ampoule No-Go Zone directly on the unshielded start->vial_A chord.
  e.n_circles = 1;
  e.circles[0].center = Vec2{0.045, -0.185};
  e.circles[0].radius = 0.022;
  e.circles[0].margin = 0.004;
  e.circles[0].active = true;

  // Polygon 0: CCW Convex Surgical Organ / Sterile Rack Virtual Fixture on the goal_A -> vial_B transit corridor.
  e.n_polygons = 1;
  e.polygons[0].n_verts = 4;
  e.polygons[0].verts[0] = Vec2{0.010, 0.055};
  e.polygons[0].verts[1] = Vec2{0.065, 0.055};
  e.polygons[0].verts[2] = Vec2{0.070, 0.095};
  e.polygons[0].verts[3] = Vec2{0.015, 0.095};
  e.polygons[0].margin = 0.003;
  e.polygons[0].active = true;
  return e;
}

SafetyShield::SafetyShield(const SafetyEnvelope& env) : env_(env) {}

void SafetyShield::reset_state() noexcept {
  prev_delta_ = Vec2{0.0, 0.0};
  has_prev_delta_ = false;
}

double SafetyShield::signed_zone_clearance(Vec2 pos, bool include_margin) const noexcept {
  double min_c = 1e9;
  for (int i = 0; i < env_.n_circles && i < kMaxNoGoCircles; ++i) {
    const auto& cz = env_.circles[i];
    if (!cz.active) continue;
    const double r_eff = cz.radius + env_.tool_radius + (include_margin ? cz.margin : 0.0);
    const double c = (pos - cz.center).norm() - r_eff;
    if (c < min_c) min_c = c;
  }
  for (int i = 0; i < env_.n_polygons && i < kMaxNoGoPolygons; ++i) {
    const auto& pz = env_.polygons[i];
    if (!pz.active || pz.n_verts < 3) continue;
    const double r_eff = env_.tool_radius + (include_margin ? pz.margin : 0.0);
    const PolyContact pc = query_convex_polygon(pz, pos);
    const double c = pc.signed_dist - r_eff;
    if (c < min_c) min_c = c;
  }
  return min_c;
}

double SafetyShield::segment_zone_clearance(Vec2 a, Vec2 b, bool include_margin) const noexcept {
  double min_c = std::min(signed_zone_clearance(a, include_margin), signed_zone_clearance(b, include_margin));
  for (int i = 0; i < env_.n_circles && i < kMaxNoGoCircles; ++i) {
    const auto& cz = env_.circles[i];
    if (!cz.active) continue;
    const double r_eff = cz.radius + env_.tool_radius + (include_margin ? cz.margin : 0.0);
    const Vec2 q = closest_on_segment(a, b, cz.center);
    const double c = (q - cz.center).norm() - r_eff;
    if (c < min_c) min_c = c;
  }
  for (int i = 0; i < env_.n_polygons && i < kMaxNoGoPolygons; ++i) {
    const auto& pz = env_.polygons[i];
    if (!pz.active || pz.n_verts < 3) continue;
    const double r_eff = env_.tool_radius + (include_margin ? pz.margin : 0.0);
    // Exact distance between segment [a, b] and every polygon edge (plus endpoint interior checks above).
    for (int k = 0; k < pz.n_verts; ++k) {
      const Vec2 e0 = pz.verts[k];
      const Vec2 e1 = pz.verts[(k + 1) % pz.n_verts];
      const double d = segment_segment_dist(a, b, e0, e1);
      const double c = d - r_eff;
      if (c < min_c) min_c = c;
    }
  }
  return min_c;
}

ShieldDecision SafetyShield::project(Vec2 current_pos, Vec2 desired_target) noexcept {
  ShieldDecision out;
  ++stats_.total_ticks;

  Vec2 p = desired_target;
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(current_pos.x) || !std::isfinite(current_pos.y)) {
    out.safe_target = std::isfinite(current_pos.x) && std::isfinite(current_pos.y) ? current_pos : Vec2{0.0, 0.0};
    out.reason_mask = kShieldNonFinite;
    ++stats_.nonfinite_clamps;
    ++stats_.interventions;
    if (hook_) hook_(hook_ctx_, out.reason_mask, stats_.interventions);
    return out;
  }

  out.raw_clearance_m = segment_zone_clearance(current_pos, desired_target, false);
  if (out.raw_clearance_m < 0.0) ++stats_.raw_zone_hazards;

  // 1. Velocity & Acceleration Kinematic Envelope Clamping on step displacement delta = p - current_pos.
  Vec2 delta = p - current_pos;
  const double dnorm = delta.norm();
  if (env_.max_step_delta > 0.0 && dnorm > env_.max_step_delta) {
    delta = delta * (env_.max_step_delta / dnorm);
    out.reason_mask |= kShieldVelocityLimit;
    ++stats_.velocity_clamps;
  }
  if (has_prev_delta_ && env_.max_step_accel > 0.0) {
    const Vec2 dv = delta - prev_delta_;
    const double dv_norm = dv.norm();
    if (dv_norm > env_.max_step_accel) {
      delta = prev_delta_ + dv * (env_.max_step_accel / dv_norm);
      out.reason_mask |= kShieldAccelerationLimit;
      ++stats_.accel_clamps;
    }
  }
  p = current_pos + delta;

  // 2. Supporting-Hyperplane Control Barrier Projection (3 alternating passes handle multi-zone intersections).
  const double wx = std::max(0.01, env_.workspace_half_x - env_.tool_radius);
  const double wy = std::max(0.01, env_.workspace_half_y - env_.tool_radius);
  const double alpha = std::clamp(env_.cbf_alpha, 0.05, 1.0);
  bool hit_ws = false, hit_circ = false, hit_poly = false;

  for (int pass = 0; pass < 3; ++pass) {
    // Workspace Virtual Fixture Box
    const double cx = std::clamp(p.x, -wx, wx);
    const double cy = std::clamp(p.y, -wy, wy);
    if (std::fabs(cx - p.x) > 1e-12 || std::fabs(cy - p.y) > 1e-12) {
      p = Vec2{cx, cy};
      hit_ws = true;
    }

    // Circular Surgical / Hazard No-Go Zones
    for (int i = 0; i < env_.n_circles && i < kMaxNoGoCircles; ++i) {
      const auto& cz = env_.circles[i];
      if (!cz.active) continue;
      const double r_eff = cz.radius + env_.tool_radius + cz.margin;
      const Vec2 d0 = current_pos - cz.center;
      const double r0 = d0.norm();
      const Vec2 n = r0 > 1e-9 ? d0 * (1.0 / r0) : Vec2{0.0, 1.0};
      // Supporting half-space at current_pos: (q - center) . n >= rhs >= r_eff guarantees the entire segment
      // [current_pos, p] remains outside B(center, r_eff) while preserving tangential slip along n.perp()!
      const double rhs = (r0 >= r_eff) ? ((1.0 - alpha) * r0 + alpha * r_eff) : (r_eff + 0.002);
      const double proj = (p - cz.center).dot(n);
      if (proj < rhs) {
        p = p + n * (rhs - proj);
        // Add gentle tangential nudge if dead-center aligned so symmetric rays glide past the virtual fixture
        const Vec2 tang = n.perp();
        if (std::fabs((p - current_pos).dot(tang)) < 0.004 && (desired_target - current_pos).norm() > 0.005) {
          const double sign = (desired_target - cz.center).dot(tang) >= 0.0 ? 1.0 : -1.0;
          p = p + tang * (0.012 * sign);
        }
        hit_circ = true;
      }
    }

    // Convex Polygonal Surgical / Fixture No-Go Zones
    for (int i = 0; i < env_.n_polygons && i < kMaxNoGoPolygons; ++i) {
      const auto& pz = env_.polygons[i];
      if (!pz.active || pz.n_verts < 3) continue;
      const double r_eff = env_.tool_radius + pz.margin;
      const PolyContact pc0 = query_convex_polygon(pz, current_pos);
      const Vec2 n = pc0.outward_normal;
      const double r0 = pc0.signed_dist;
      const double rhs = (r0 >= r_eff) ? ((1.0 - alpha) * r0 + alpha * r_eff) : (r_eff + 0.002);
      const double proj = (p - pc0.boundary_pt).dot(n);
      if (proj < rhs) {
        p = p + n * (rhs - proj);
        const Vec2 tang = n.perp();
        if (std::fabs((p - current_pos).dot(tang)) < 0.004 && (desired_target - current_pos).norm() > 0.005) {
          const double sign = (desired_target - pc0.boundary_pt).dot(tang) >= 0.0 ? 1.0 : -1.0;
          p = p + tang * (0.012 * sign);
        }
        hit_poly = true;
      }
    }
  }

  // Final fail-safe invariant check: if opposing constraints conflict in a corner, hold safe current_pos.
  if (segment_zone_clearance(current_pos, p, false) < 0.0 && signed_zone_clearance(current_pos, false) >= 0.0) {
    p = current_pos;
    hit_circ = true;
  }

  if (hit_ws) { out.reason_mask |= kShieldWorkspaceBound; ++stats_.workspace_clamps; }
  if (hit_circ) { out.reason_mask |= kShieldCircleNoGo; ++stats_.circle_projections; }
  if (hit_poly) { out.reason_mask |= kShieldPolygonNoGo; ++stats_.polygon_projections; }

  prev_delta_ = p - current_pos;
  has_prev_delta_ = true;

  out.safe_target = p;
  out.min_clearance_m = segment_zone_clearance(current_pos, p, false);
  if (out.min_clearance_m < stats_.min_clearance_seen_m) stats_.min_clearance_seen_m = out.min_clearance_m;

  if (out.intervened()) {
    ++stats_.interventions;
    if (hook_) hook_(hook_ctx_, out.reason_mask, stats_.interventions);
  }
  return out;
}

Obs ShieldedRobotAdapter::step(Vec2 target_xy) {
  const Vec2 cur = inner_.robot_pose();
  last_desired_ = target_xy;
  last_dec_ = shield_.project(cur, target_xy);
  const Vec2 cmd = enforce_ ? last_dec_.safe_target : target_xy;
  Obs obs = inner_.step(cmd);
  const Vec2 next_pos = inner_.robot_pose();
  const double seg_c = shield_.segment_zone_clearance(cur, next_pos, false);
  if (seg_c < min_true_clearance_) min_true_clearance_ = seg_c;
  if (seg_c < -1e-7) ++intrusions_;
  last_pos_ = next_pos;
  return obs;
}

void ShieldedRobotAdapter::move_robot_to(Vec2 pose, int steps) {
  for (int k = 0; k < steps; ++k) {
    step(pose);
    if ((inner_.robot_pose() - pose).norm() < 0.01) break;
  }
}

}  // namespace relmw
