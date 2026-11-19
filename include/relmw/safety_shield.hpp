// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// 1 kHz Simplex Physical Safety Shield (Runtime Assurance / Control Barrier Projection).
//
// Designed for the QNX 8.0 1 ms pulse-driven control loop:
//   * O(1) deterministic time per tick, ZERO heap allocations on the real-time path (verified in test_rt_noalloc).
//   * Hard velocity & acceleration clamping (kinematic safety envelope).
//   * Dynamic circular & convex-polygonal No-Go Zones / Virtual Fixtures (Robotics & Medical surgical / sterile
//     pharmacy keep-out regions) enforced via supporting-hyperplane Control Barrier Function (CBF) projection.
//   * Preserves tangential velocity along obstacle boundaries so the end-effector slides smoothly around virtual
//     fixtures instead of deadlocking or penetrating, and emits deterministic SHIELD_INTERVENTION pulse counts.
#pragma once
#include "relmw/robot.hpp"
#include "relmw/sim2d.hpp"
#include <cstdint>
#include <type_traits>

namespace relmw {

constexpr int kMaxNoGoCircles = 4;
constexpr int kMaxNoGoPolygons = 2;
constexpr int kMaxPolyVerts = 8;

enum ShieldReasonBits : std::uint32_t {
  kShieldNone              = 0u,
  kShieldNonFinite         = 1u << 0,
  kShieldWorkspaceBound    = 1u << 1,
  kShieldCircleNoGo        = 1u << 2,
  kShieldPolygonNoGo       = 1u << 3,
  kShieldVelocityLimit     = 1u << 4,
  kShieldAccelerationLimit = 1u << 5,
};

struct CircleNoGoZone {
  Vec2 center{0.0, 0.0};
  double radius = 0.0;        // physical hazard radius (m)
  double margin = 0.004;      // additional safety margin beyond tool radius (m)
  bool active = false;
};

// Counter-clockwise convex polygon virtual fixture (up to kMaxPolyVerts vertices, inline fixed storage).
struct ConvexPolygonZone {
  Vec2 verts[kMaxPolyVerts]{};
  int n_verts = 0;
  double margin = 0.004;      // additional safety margin beyond tool radius (m)
  bool active = false;
};

struct SafetyEnvelope {
  double workspace_half_x = kTableX;     // m
  double workspace_half_y = kTableY;     // m
  double tool_radius = kPusherR;         // m (end-effector radius)
  double max_step_delta = 0.045;         // max position target lead / velocity envelope per control step (m)
  double max_step_accel = 0.030;         // max change in step displacement vector per tick (m)
  double cbf_alpha = 0.45;               // discrete-time CBF barrier relaxation rate in (0, 1]
  int n_circles = 0;
  CircleNoGoZone circles[kMaxNoGoCircles]{};
  int n_polygons = 0;
  ConvexPolygonZone polygons[kMaxNoGoPolygons]{};

  // Standard industrial cell envelope (workspace + kinematic limits only).
  static SafetyEnvelope industrial_default();

  // Robotics & Medical surgical / sterile pharmacy fixture preset:
  //   * Circle 0: fragile vascular / ampoule No-Go Zone at (0.045, -0.185), r=2.2 cm
  //   * Polygon 0: sterile instrument tray / organ Virtual Fixture quad near (0.02, 0.07)
  static SafetyEnvelope medical_surgical_fixture();
};

struct ShieldDecision {
  Vec2 safe_target{0.0, 0.0};
  std::uint32_t reason_mask = kShieldNone;
  double min_clearance_m = 1.0;          // signed clearance to nearest No-Go boundary after projection (m)
  double raw_clearance_m = 1.0;          // signed clearance of the unshielded desired target (m; <0 means violation)
  bool intervened() const { return reason_mask != kShieldNone; }
};

struct ShieldStats {
  std::uint64_t total_ticks = 0;
  std::uint64_t interventions = 0;       // SHIELD_INTERVENTION pulse count
  std::uint64_t circle_projections = 0;
  std::uint64_t polygon_projections = 0;
  std::uint64_t workspace_clamps = 0;
  std::uint64_t velocity_clamps = 0;
  std::uint64_t accel_clamps = 0;
  std::uint64_t nonfinite_clamps = 0;
  std::uint64_t raw_zone_hazards = 0;    // times unshielded command would have entered/crossed a No-Go Zone
  double min_clearance_seen_m = 1e9;
};

static_assert(std::is_trivially_copyable<CircleNoGoZone>::value, "CircleNoGoZone must be trivially copyable");
static_assert(std::is_trivially_copyable<ConvexPolygonZone>::value, "ConvexPolygonZone must be trivially copyable");
static_assert(std::is_trivially_copyable<SafetyEnvelope>::value, "SafetyEnvelope must be trivially copyable");
static_assert(std::is_trivially_copyable<ShieldDecision>::value, "ShieldDecision must be trivially copyable");
static_assert(std::is_trivially_copyable<ShieldStats>::value, "ShieldStats must be trivially copyable");

// Optional raw function pointer invoked on SHIELD_INTERVENTION (no std::function -> zero heap allocation).
using ShieldPulseHook = void (*)(void* ctx, std::uint32_t reason_mask, std::uint64_t total_interventions);

class SafetyShield {
 public:
  explicit SafetyShield(const SafetyEnvelope& env = SafetyEnvelope::medical_surgical_fixture());

  void set_envelope(const SafetyEnvelope& env) noexcept { env_ = env; }
  const SafetyEnvelope& envelope() const noexcept { return env_; }

  void set_pulse_hook(ShieldPulseHook hook, void* ctx = nullptr) noexcept {
    hook_ = hook;
    hook_ctx_ = ctx;
  }

  void reset_state() noexcept;
  void reset_stats() noexcept { stats_ = ShieldStats{}; }

  // O(1), allocation-free Control Barrier Projection on the 1 kHz real-time path.
  ShieldDecision project(Vec2 current_pos, Vec2 desired_target) noexcept;

  // Signed clearance from tool center `pos` to the union of active No-Go Zones (excluding margin if include_margin=false).
  // < 0 means physical tool penetration into a forbidden surgical/fixture zone.
  double signed_zone_clearance(Vec2 pos, bool include_margin = false) const noexcept;

  // Minimum signed clearance along the continuous chord segment [a, b].
  double segment_zone_clearance(Vec2 a, Vec2 b, bool include_margin = false) const noexcept;

  const ShieldStats& stats() const noexcept { return stats_; }

 private:
  SafetyEnvelope env_;
  ShieldStats stats_{};
  Vec2 prev_delta_{0.0, 0.0};
  bool has_prev_delta_ = false;
  ShieldPulseHook hook_ = nullptr;
  void* hook_ctx_ = nullptr;
};

// Decorator RobotAdapter that enforces the 1 kHz Simplex SafetyShield on every step() and move_robot_to() command,
// and tracks ground-truth physical intrusions on the underlying robot trajectory (both when enabled and in audit-only
// mode so SinglePass / NaiveRetry unshielded baselines can be measured with identical geometry).
class ShieldedRobotAdapter : public RobotAdapter {
 public:
  ShieldedRobotAdapter(RobotAdapter& inner, SafetyShield& shield, bool enforce = true)
      : inner_(inner), shield_(shield), enforce_(enforce), last_pos_(inner.robot_pose()) {}

  Obs get_obs() override { return inner_.get_obs(); }
  Vec2 robot_pose() override { return inner_.robot_pose(); }
  Obs step(Vec2 target_xy) override;
  void move_robot_to(Vec2 pose, int steps = 60) override;

  bool enforcing() const noexcept { return enforce_; }
  void set_enforcing(bool e) noexcept { enforce_ = e; }
  std::uint64_t zone_intrusions() const noexcept { return intrusions_; }
  double min_true_clearance_m() const noexcept { return min_true_clearance_; }
  const ShieldDecision& last_decision() const noexcept { return last_dec_; }
  Vec2 last_desired() const noexcept { return last_desired_; }
  Vec2 last_pose() const noexcept { return last_pos_; }

 private:
  RobotAdapter& inner_;
  SafetyShield& shield_;
  bool enforce_ = true;
  Vec2 last_pos_{0.0, 0.0};
  Vec2 last_desired_{0.0, 0.0};
  ShieldDecision last_dec_{};
  std::uint64_t intrusions_ = 0;
  double min_true_clearance_ = 1e9;
};

}  // namespace relmw
