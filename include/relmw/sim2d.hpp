// Deterministic 2-D quasi-static pushing simulator (no GPU, no dynamic allocation in step()).
// Same task as the MuJoCo Python demo: a disk pusher pushes cube A to goal A and cube B to goal B.
// Field faults (drawn per episode seed, documented causes):
//   bump        : one external knock displaces a cube 8-14 cm mid-push (transient -> independent of retries)
//   percep_bias : the robot's own object estimate is offset 4-7 cm (drifted extrinsics; persistent ->
//                 correlated across retries). The middleware's verifier camera is unaffected.
//   slippery    : cube friction x0.15 (oily part) -> slides past where it was pushed; persistent.
#pragma once
#include "relmw/robot.hpp"
#include "relmw/rng.hpp"
#include <cstdint>

namespace relmw {

enum class Fault { None, Bump, PercepBias, Slippery };
const char* to_string(Fault f);

struct SimConfig {
  bool field_faults = false;          // false = "nominal" condition (training distribution)
  double jitter = 0.03;               // half-width of cube start noise (m)
  double w_none = 0.30, w_bump = 0.25, w_percep = 0.25, w_slip = 0.20;
  int force_fault = -1;               // >=0: override the drawn fault (tests / demo)
};

constexpr double kTableX = 0.5, kTableY = 0.4;
constexpr double kGoalTol = 0.04;     // success radius (m)
constexpr double kPusherR = 0.02, kCubeR = 0.025;
constexpr double kTick = 0.05;        // control period of one step() in simulated seconds (20 Hz policy)
constexpr int kSubsteps = 10;
Vec2 goal_of(int obj);

class PushSim2D : public RobotAdapter {
 public:
  PushSim2D(std::uint64_t seed, const SimConfig& cfg = {});
  Obs get_obs() override;
  Obs step(Vec2 target_xy) override;
  Vec2 robot_pose() override { return pusher_; }
  void move_robot_to(Vec2 pose, int steps = 60) override;

  // Ground truth (evaluation / verifier camera only; never given to the policy).
  Obs true_state() const;
  Fault fault() const { return fault_; }
  long ticks() const { return t_; }
  double friction() const { return mu_; }
  // Inject a knock now (used by the live supervisor demo: `echo bump > /dev/relmw/ctl`).
  void knock(int obj, Vec2 delta);
  void set_percep_bias(Vec2 b) { percep_bias_ = b; }

 private:
  void substep(Vec2 target, double dt);
  SimConfig cfg_;
  Rng rng_;
  Fault fault_ = Fault::None;
  double mu_ = 0.6;
  Vec2 pusher_, pvel_;
  Vec2 obj_[kNumObjects], ovel_[kNumObjects];
  Vec2 percep_bias_;
  int bump_obj_ = 0, bump_t_ = 30, contact_steps_ = 0;
  Vec2 bump_vec_;
  bool bumped_ = false;
  long t_ = 0;
};

}  // namespace relmw
