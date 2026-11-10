#include "relmw/sim2d.hpp"
#include <algorithm>
#include <cmath>

namespace relmw {

const char* to_string(Fault f) {
  switch (f) {
    case Fault::None: return "none";
    case Fault::Bump: return "bump";
    case Fault::PercepBias: return "percep_bias";
    case Fault::Slippery: return "slippery";
  }
  return "?";
}

Vec2 goal_of(int obj) { return obj == 0 ? Vec2{0.25, 0.20} : Vec2{-0.25, 0.20}; }

PushSim2D::PushSim2D(std::uint64_t seed, const SimConfig& cfg) : cfg_(cfg), rng_(hash_mix({0x51u, seed})) {
  if (cfg_.field_faults) {
    const double w[4] = {cfg_.w_none, cfg_.w_bump, cfg_.w_percep, cfg_.w_slip};
    fault_ = static_cast<Fault>(rng_.choice_weighted(w, 4));
  }
  if (cfg_.force_fault >= 0) fault_ = static_cast<Fault>(cfg_.force_fault);
  mu_ = 0.6 * rng_.uniform(0.85, 1.15) * (fault_ == Fault::Slippery ? 0.15 : 1.0);
  double ang = rng_.uniform(0, 2 * M_PI), mag = rng_.uniform(0.04, 0.07);
  if (fault_ == Fault::PercepBias) percep_bias_ = Vec2{std::cos(ang), std::sin(ang)} * mag;
  bump_obj_ = rng_.uniform() < 0.5 ? 0 : 1;
  bump_t_ = rng_.randint(15, 45);
  double bang = rng_.uniform(0, 2 * M_PI);
  bump_vec_ = Vec2{std::cos(bang), std::sin(bang)} * rng_.uniform(0.08, 0.14);
  pusher_ = {0.0, -0.30};
  obj_[0] = {0.12, -0.02};
  obj_[1] = {-0.12, -0.02};
  for (auto& o : obj_) o += Vec2{rng_.uniform(-cfg_.jitter, cfg_.jitter), rng_.uniform(-cfg_.jitter, cfg_.jitter)};
}

Obs PushSim2D::true_state() const {
  Obs o; o.pusher = pusher_;
  for (int i = 0; i < kNumObjects; ++i) o.obj[i] = obj_[i];
  return o;
}

Obs PushSim2D::get_obs() {
  Obs o = true_state();
  for (int i = 0; i < kNumObjects; ++i) o.obj[i] += percep_bias_;
  return o;
}

void PushSim2D::substep(Vec2 target, double dt) {
  // Pusher: position servo with speed limit (like a PD-controlled gantry).
  Vec2 err = target - pusher_;
  Vec2 v = err * (1.0 / 0.04);                   // reach target in ~40 ms
  double vmax = 0.30, sp = v.norm();
  if (sp > vmax) v = v * (vmax / sp);
  pusher_ += v * dt;
  pvel_ = v;
  // Cubes: Coulomb sliding friction decelerates free sliding; contact pushes cube out of the pusher.
  for (int i = 0; i < kNumObjects; ++i) {
    double s = ovel_[i].norm();
    if (s > 0) {
      double ds = mu_ * 9.81 * dt;
      ovel_[i] = s <= ds ? Vec2{} : ovel_[i] * ((s - ds) / s);
    }
    obj_[i] += ovel_[i] * dt;
    Vec2 d = obj_[i] - pusher_;
    double dist = d.norm(), rs = kPusherR + kCubeR;
    if (dist < rs) {
      Vec2 n = dist > 1e-9 ? d * (1.0 / dist) : Vec2{0, 1};
      obj_[i] = pusher_ + n * rs;
      double vn = pvel_.dot(n);
      if (vn > ovel_[i].dot(n)) ovel_[i] = ovel_[i] + n * (vn - ovel_[i].dot(n));
    }
  }
  // Cube-cube contact (rare): separate symmetrically.
  Vec2 d = obj_[1] - obj_[0];
  double dist = d.norm();
  if (dist < 2 * kCubeR && dist > 1e-9) {
    Vec2 n = d * (1.0 / dist);
    double pen = 2 * kCubeR - dist;
    obj_[0] = obj_[0] - n * (pen / 2);
    obj_[1] = obj_[1] + n * (pen / 2);
  }
}

Obs PushSim2D::step(Vec2 target) {
  double dt = kTick / kSubsteps;
  for (int k = 0; k < kSubsteps; ++k) substep(target, dt);
  ++t_;
  if (fault_ == Fault::Bump && !bumped_) {
    if ((pusher_ - obj_[bump_obj_]).norm() < 0.06) {
      if (++contact_steps_ >= bump_t_) { knock(bump_obj_, bump_vec_); bumped_ = true; }
    }
  }
  return get_obs();
}

void PushSim2D::knock(int obj, Vec2 delta) {
  obj_[obj] += delta;
  for (auto& v : ovel_) v = Vec2{};
}

void PushSim2D::move_robot_to(Vec2 pose, int steps) {
  for (int k = 0; k < steps; ++k) {
    step(pose);
    if ((pusher_ - pose).norm() < 0.01) break;
  }
}

}  // namespace relmw
