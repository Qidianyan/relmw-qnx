#include "relmw/policy.hpp"
#include "relmw/rng.hpp"
#include <algorithm>
#include <cmath>

namespace relmw {

Vec2 expert_action(const Obs& obs, int obj, Vec2 goal, double bias) {
  Vec2 b = obs.obj[obj];
  Vec2 dirv = (goal - b).unit();
  double c = std::cos(bias), s = std::sin(bias);
  Vec2 dirv_b{c * dirv.x - s * dirv.y, s * dirv.x + c * dirv.y};
  Vec2 perp = dirv.perp();
  Vec2 p = obs.pusher;
  double along = (p - b).dot(dirv), lat = (p - b).dot(perp);
  if (along > -0.03) {                                   // wrong side: circle around
    double side = lat >= 0 ? 1.0 : -1.0;
    return b + perp * (0.09 * side) - dirv * 0.07;
  }
  double w = std::clamp((std::fabs(lat) - 0.006) / 0.02, 0.0, 1.0);
  Vec2 lineup = b - dirv * 0.07;
  Vec2 push = p + dirv_b * 0.025 - perp * (lat * 0.5);
  return push * (1 - w) + lineup * w;
}

Vec2 ScriptedPushPolicy::act(const Obs& obs, const std::string& subtask, const SubtaskMeta& meta, const Perturb& p) {
  // FNV-1a of the subtask name (std::hash differs across libcs; this is identical on Linux and QNX).
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char ch : subtask) { h ^= ch; h *= 1099511628211ULL; }
  Rng r(hash_mix({base_seed_, ep_, h, p.has_seed ? p.seed + 1 : 0}));
  double bias = bias_std_ > 0 ? r.normal(0, bias_std_) : 0.0;
  return expert_action(obs, meta.object, meta.goal_xy, bias);
}

}  // namespace relmw
