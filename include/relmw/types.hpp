// Basic value types shared by the portable core.
#pragma once
#include <cmath>
#include <string>

namespace relmw {

struct Vec2 {
  double x = 0, y = 0;
  Vec2() = default;
  Vec2(double x_, double y_) : x(x_), y(y_) {}
  Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(double s) const { return {x * s, y * s}; }
  Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
  double dot(Vec2 o) const { return x * o.x + y * o.y; }
  double norm() const { return std::sqrt(x * x + y * y); }
  Vec2 perp() const { return {-y, x}; }          // +90 deg
  Vec2 unit() const { double n = norm() + 1e-9; return {x / n, y / n}; }
};

constexpr int kNumObjects = 2;                     // object 0 = "A" (red), 1 = "B" (blue)
inline const char* object_name(int i) { return i == 0 ? "A" : "B"; }

// What the robot's own perception stack reports (may be biased by a fault).
struct Obs {
  Vec2 pusher;
  Vec2 obj[kNumObjects];
};

enum class FailureMode { None, LostContact, WrongSideStall, StoppedShort, OffAxisDrift, Unrecoverable, Regressed };

inline const char* to_string(FailureMode m) {
  switch (m) {
    case FailureMode::None: return "none";
    case FailureMode::LostContact: return "lost_contact";
    case FailureMode::WrongSideStall: return "wrong_side_stall";
    case FailureMode::StoppedShort: return "stopped_short";
    case FailureMode::OffAxisDrift: return "off_axis_drift";
    case FailureMode::Unrecoverable: return "unrecoverable";
    case FailureMode::Regressed: return "regressed";
  }
  return "?";
}

// Verifier verdict: what the middleware believes about a subtask postcondition.
struct Verdict {
  bool ok = false;
  FailureMode mode = FailureMode::None;
  bool has_obj = false;     // verifier produced an independent object estimate
  Vec2 obj_xy;              // that estimate (used to fingerprint failures / relocalize)
  double dist = 0;          // estimated distance to goal (m)
};

}  // namespace relmw
