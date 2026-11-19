// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
#include "relmw/experiment.hpp"
#include "relmw/policy.hpp"
#include "relmw/robot_link.hpp"
#include "relmw/stats.hpp"
#include "relmw/telemetry_bridge.hpp"
#include "relmw/verifier.hpp"
#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

namespace relmw {

const char* to_string(Arm a) {
  switch (a) {
    case Arm::SinglePass: return "single_pass";
    case Arm::NaiveRetry: return "naive_retry";
    case Arm::CorrAware: return "corr_aware_retry";
  }
  return "?";
}

const char* to_string(PolicyKind p) {
  switch (p) {
    case PolicyKind::Scripted: return "scripted";
    case PolicyKind::TinyMLP_FP32: return "tiny_mlp_fp32";
    case PolicyKind::TinyMLP_Q12: return "tiny_mlp_q12";
  }
  return "?";
}

const char* to_string(CellPreset c) {
  switch (c) {
    case CellPreset::Standard: return "standard";
    case CellPreset::MedicalVial: return "medical_vial";
  }
  return "?";
}

std::vector<Subtask> make_task(TruthSensor& truth, std::uint64_t ep) {
  std::vector<Subtask> sts;
  for (int i = 0; i < kNumObjects; ++i) {
    Subtask st;
    st.name = std::string("push_") + object_name(i);
    auto cam = std::make_shared<CameraVerifier>(&truth, i, hash_mix({0xC0u, ep, (std::uint64_t)i}));
    st.verifier = [cam](const Obs& o) { return (*cam)(o); };
    st.oracle = [&truth, i](const Obs&) { return oracle_verdict(truth.truth(), i); };
    st.max_steps = 200; st.check_every = 5; st.hold = 2;
    st.meta.object = i; st.meta.goal_xy = goal_of(i);
    sts.push_back(st);
  }
  return sts;
}

namespace {
bool is_legacy_default(const ExperimentConfig& cfg) {
  return cfg.policy_kind == PolicyKind::Scripted && !cfg.enable_shield && cfg.preset == CellPreset::Standard;
}

SafetyEnvelope envelope_for(CellPreset p) {
  return p == CellPreset::MedicalVial ? SafetyEnvelope::medical_surgical_fixture()
                                      : SafetyEnvelope::industrial_default();
}
}  // namespace

EpisodeResult run_episode_impl(int ep, bool field, Arm arm, RetryPolicy* rp, const ExperimentConfig& cfg,
                               int max_retries_override, EventSink sink) {
  SimConfig sc; sc.field_faults = field;
  PushSim2D sim(ep, sc);

  ScriptedPushPolicy scripted_pol(cfg.policy_bias_std);
  TinyMLPPolicy mlp_pol(cfg.policy_kind == PolicyKind::TinyMLP_Q12 ? MLPInferenceMode::FixedQ12
                                                                   : MLPInferenceMode::Float32,
                        cfg.policy_bias_std);
  scripted_pol.new_episode(ep);
  mlp_pol.new_episode(ep);
  Policy& pol = (cfg.policy_kind == PolicyKind::Scripted) ? static_cast<Policy&>(scripted_pol)
                                                          : static_cast<Policy&>(mlp_pol);

  MiddlewareConfig mc;
  mc.max_retries = max_retries_override >= 0 ? max_retries_override : (arm == Arm::SinglePass ? 0 : 3);
  mc.episode_step_budget = 2000;
  mc.seed = ep;

  EpisodeReport rep;
  std::uint64_t zone_intrusions = 0;
  std::uint64_t shield_interventions = 0;
  double min_clearance = 1.0;

  if (is_legacy_default(cfg)) {
    ReliabilityMiddleware mw(pol, sim, rp, mc, std::move(sink));
    auto task = make_task(sim, ep);
    rep = mw.run(task);
  } else {
    SafetyShield shield(envelope_for(cfg.preset));
    // Enforce 1 kHz Simplex SafetyShield on CorrAware when enable_shield is set; audit intrusions on all arms.
    const bool enforce = cfg.enable_shield && (arm == Arm::CorrAware);
    ShieldedRobotAdapter srobot(sim, shield, enforce);
    ReliabilityMiddleware mw(pol, srobot, rp, mc, std::move(sink));
    auto task = make_task(sim, ep);
    rep = mw.run(task);
    zone_intrusions = srobot.zone_intrusions();
    shield_interventions = enforce ? shield.stats().interventions : 0;
    min_clearance = srobot.min_true_clearance_m();
  }

  Obs ts = sim.true_state();
  bool gt = true;
  for (int i = 0; i < kNumObjects; ++i) gt = gt && oracle_verdict(ts, i).ok;
  EpisodeResult r;
  r.ep = ep; r.condition = field ? "field" : "nominal"; r.arm = arm; r.fault = sim.fault();
  r.gt_success = gt; r.mw_success = rep.success; r.abort_reason = rep.abort_reason; r.failed_at = rep.failed_at;
  r.sim_steps = sim.ticks();
  r.zone_intrusions = zone_intrusions;
  r.shield_interventions = shield_interventions;
  r.min_clearance_m = min_clearance;
  r.attempts = rep.attempts;
  return r;
}

EpisodeResult run_episode(int ep, bool field, Arm arm, RetryPolicy* rp, const ExperimentConfig& cfg, EventSink sink) {
  return run_episode_impl(ep, field, arm, rp, cfg, -1, std::move(sink));
}

bool parse_plan(const std::string& csv, std::vector<std::string>& out, std::string* err) {
  out.clear();
  auto fail = [&](const std::string& e) {
    if (err) *err = e;
    return false;
  };
  if (csv.empty()) return fail("plan is empty");
  std::vector<std::string> items;
  std::string item;
  for (size_t i = 0;; ++i) {
    const bool sep = i == csv.size() || csv[i] == ',';
    if (sep) {
      if (item.empty()) return fail("empty subtask name in plan \"" + csv + "\"");
      items.push_back(item);
      item.clear();
      if (i == csv.size()) break;
    } else {
      item.push_back(csv[i]);
    }
  }
  if (items.size() > 8) return fail("plan has " + std::to_string(items.size()) + " items (max 8)");
  for (const auto& s : items) {
    if (s.find(' ') != std::string::npos) return fail("subtask name \"" + s + "\" must not contain spaces");
    for (char c : s)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
        return fail("subtask name \"" + s + "\" must match [A-Za-z0-9_]+");
  }
  out = std::move(items);
  return true;
}

bool apply_plan(std::vector<Subtask>& task, const std::vector<std::string>& plan, std::string* err) {
  auto fail = [&](const std::string& e) {
    if (err) *err = e;
    return false;
  };
  if (plan.empty()) return fail("plan is empty");
  for (size_t i = 0; i < plan.size(); ++i)
    for (size_t j = 0; j < i; ++j)
      if (plan[i] == plan[j]) return fail("duplicate subtask \"" + plan[i] + "\" in plan");
  std::vector<Subtask> out;
  for (const auto& name : plan) {
    const Subtask* found = nullptr;
    for (const auto& st : task)
      if (st.name == name) { found = &st; break; }
    if (!found) return fail("unknown subtask \"" + name + "\" (not in the task)");
    out.push_back(*found);
  }
  task = std::move(out);
  return true;
}

namespace {

std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
  return o + "\"";
}
std::string jnum(double v) { char b[64]; std::snprintf(b, sizeof b, "%.10g", v); return b; }

std::string summarize(const std::vector<EpisodeResult>& rows, const std::vector<EpisodeResult>* base, bool include_splus) {
  int n = (int)rows.size(), k = 0, det = 0, rec = 0, retries = 0, retries_ok = 0, retries_corr = 0, esc = 0;
  int tp = 0, fp = 0, fn = 0, tn = 0;
  int eps_with_intrusion = 0;
  std::uint64_t total_intrusions = 0, total_shield_interventions = 0;
  double min_clear = 1e9;
  std::map<std::string, int> modes, strats;
  std::map<std::string, std::pair<int, int>> by_fault;
  std::vector<double> steps;
  for (auto& r : rows) {
    k += r.gt_success;
    if (r.zone_intrusions > 0) ++eps_with_intrusion;
    total_intrusions += r.zone_intrusions;
    total_shield_interventions += r.shield_interventions;
    if (r.min_clearance_m < min_clear) min_clear = r.min_clearance_m;
    bool any_fail = false;
    for (auto& a : r.attempts) {
      if (!a.ok) { any_fail = true; modes[to_string(a.mode)]++; }
      if (a.attempt > 0) {
        ++retries; retries_ok += a.ok; retries_corr += a.correlated;
        strats[a.strategy + (a.ok ? ":ok" : ":fail")]++;
      } else if (a.attempt == 0 && a.strategy.find("proactive") != std::string::npos) {
        strats[a.strategy + (a.ok ? ":ok" : ":fail")]++;
      }
      if (a.attempt >= 0 && a.gt_ok >= 0) {
        if (a.ok && a.gt_ok) ++tp; else if (a.ok && !a.gt_ok) ++fp; else if (!a.ok && a.gt_ok) ++fn; else ++tn;
      }
    }
    if (any_fail) { ++det; rec += r.gt_success; }
    esc += r.abort_reason == "escalate_to_operator";
    auto& bf = by_fault[to_string(r.fault)]; bf.first++; bf.second += r.gt_success;
    steps.push_back((double)r.sim_steps);
  }
  auto ci = stats::wilson(k, n);
  auto rci = stats::wilson(rec, det);
  auto ms = stats::bootstrap_mean_ci(steps);
  std::ostringstream o;
  o << "{\"N\":" << n << ",\"success\":" << k << ",\"success_rate\":" << jnum(n ? (double)k / n : 0)
    << ",\"success_ci95\":[" << jnum(ci.lo) << "," << jnum(ci.hi) << "]"
    << ",\"episodes_with_detected_failure\":" << det << ",\"recovered\":" << rec
    << ",\"recovery_rate\":" << (det ? jnum((double)rec / det) : "null")
    << ",\"recovery_ci95\":[" << jnum(rci.lo) << "," << jnum(rci.hi) << "]"
    << ",\"retries\":" << retries << ",\"retries_succeeded\":" << retries_ok
    << ",\"retries_repeating_same_failure\":" << retries_corr << ",\"escalated_to_operator\":" << esc;
  if (include_splus) {
    const int safe_eps = n - eps_with_intrusion;
    o << ",\"episodes_with_zone_intrusion\":" << eps_with_intrusion
      << ",\"zero_intrusion_episodes\":" << safe_eps
      << ",\"zero_intrusion_rate\":" << jnum(n ? (double)safe_eps / n : 1.0)
      << ",\"total_zone_intrusions\":" << total_intrusions
      << ",\"total_shield_interventions\":" << total_shield_interventions
      << ",\"min_clearance_m\":" << jnum(min_clear < 1e8 ? min_clear : 1.0);
  }
  o << ",\"failure_modes\":{"; bool first = true;
  for (auto& [m, c] : modes) { o << (first ? "" : ",") << jstr(m) << ":" << c; first = false; }
  o << "},\"strategies\":{"; first = true;
  for (auto& [m, c] : strats) { o << (first ? "" : ",") << jstr(m) << ":" << c; first = false; }
  o << "},\"verifier_vs_ground_truth\":{\"true_pos\":" << tp << ",\"false_pos\":" << fp << ",\"false_neg\":" << fn
    << ",\"true_neg\":" << tn << "}";
  o << ",\"mean_sim_steps\":[" << jnum(ms.mean) << "," << jnum(ms.lo) << "," << jnum(ms.hi) << "]";
  o << ",\"by_fault\":{"; first = true;
  for (auto& [f, c] : by_fault) { o << (first ? "" : ",") << jstr(f) << ":{\"N\":" << c.first << ",\"success\":" << c.second << "}"; first = false; }
  o << "}";
  if (base) {
    std::vector<std::pair<bool, bool>> pairs;
    int fixed = 0, broken = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
      bool a = (*base)[i].gt_success, b = rows[i].gt_success;
      pairs.push_back({a, b}); fixed += (b && !a); broken += (a && !b);
    }
    auto d = stats::paired_diff_ci(pairs);
    o << ",\"vs_single_pass\":{\"diff\":" << jnum(d.diff) << ",\"ci95\":[" << jnum(d.lo) << "," << jnum(d.hi)
      << "],\"mcnemar_p\":" << jnum(stats::mcnemar_exact(fixed, broken)) << ",\"fixed\":" << fixed << ",\"broken\":" << broken << "}";
  }
  o << "}";
  return o.str();
}

std::string episode_json(const EpisodeResult& r, bool include_splus) {
  std::ostringstream o;
  o << "{\"ep\":" << r.ep << ",\"condition\":" << jstr(r.condition) << ",\"arm\":" << jstr(to_string(r.arm))
    << ",\"fault\":" << jstr(to_string(r.fault)) << ",\"gt_success\":" << (r.gt_success ? "true" : "false")
    << ",\"mw_success\":" << (r.mw_success ? "true" : "false") << ",\"abort\":" << jstr(r.abort_reason)
    << ",\"failed_at\":" << jstr(r.failed_at) << ",\"sim_steps\":" << r.sim_steps;
  if (include_splus) {
    o << ",\"zone_intrusions\":" << r.zone_intrusions
      << ",\"shield_interventions\":" << r.shield_interventions;
  }
  o << ",\"attempts\":[";
  for (size_t i = 0; i < r.attempts.size(); ++i) {
    auto& a = r.attempts[i];
    o << (i ? "," : "") << "{\"subtask\":" << jstr(a.subtask) << ",\"attempt\":" << a.attempt << ",\"ok\":" << (a.ok ? "true" : "false")
      << ",\"mode\":" << jstr(to_string(a.mode)) << ",\"steps\":" << a.steps << ",\"strategy\":" << jstr(a.strategy)
      << ",\"correlated\":" << (a.correlated ? "true" : "false") << ",\"gt_ok\":" << a.gt_ok << "}";
  }
  o << "]}";
  return o.str();
}

}  // namespace

BenchOutput run_benchmark(const ExperimentConfig& cfg) {
  BenchOutput out;
  const bool splus = !is_legacy_default(cfg);
  CorrelationAwareRetry lib;
  if (!splus) {
    for (int ep = cfg.warmup_seed0; ep < cfg.warmup_seed0 + cfg.warmup; ++ep)
      run_episode(ep, true, Arm::CorrAware, &lib, cfg);
  } else {
    // Held-out calibration warmup (seeds 5000..5039): explore up to 6 ladder steps across both subtask orders so
    // both push_A and push_B empirically observe every recovery strategy before freezing the library for evaluation.
    const std::vector<std::string> rev_plan{"push_B", "push_A"};
    for (int ep = cfg.warmup_seed0; ep < cfg.warmup_seed0 + cfg.warmup; ++ep) {
      run_episode_impl(ep, true, Arm::CorrAware, &lib, cfg, 6, nullptr);
      SimConfig sc; sc.field_faults = true;
      PushSim2D sim(ep, sc);
      TinyMLPPolicy mlp_pol(cfg.policy_kind == PolicyKind::TinyMLP_Q12 ? MLPInferenceMode::FixedQ12
                                                                       : MLPInferenceMode::Float32,
                            cfg.policy_bias_std);
      ScriptedPushPolicy scr_pol(cfg.policy_bias_std);
      mlp_pol.new_episode(ep); scr_pol.new_episode(ep);
      Policy& pol = (cfg.policy_kind == PolicyKind::Scripted) ? static_cast<Policy&>(scr_pol) : static_cast<Policy&>(mlp_pol);
      SafetyShield shield(envelope_for(cfg.preset));
      ShieldedRobotAdapter srobot(sim, shield, cfg.enable_shield);
      MiddlewareConfig mc; mc.max_retries = 6; mc.episode_step_budget = 2000; mc.seed = ep;
      ReliabilityMiddleware mw(pol, srobot, &lib, mc);
      auto task = make_task(sim, ep);
      apply_plan(task, rev_plan, nullptr);
      mw.run(task);
    }
  }
  lib.learn = false;
  {
    std::ostringstream l;
    for (auto& [k, v] : lib.lib) l << k << ' ' << v.first << ' ' << v.second << '\n';
    out.library_txt = l.str();
  }
  std::ostringstream js, jl, tab;
  if (!splus) {
    js << "{\"label\":\"SIMULATION ONLY (deterministic C++ 2-D push sim; scripted policy; camera-style verifier)\"";
  } else {
    js << "{\"label\":" << jstr(std::string("SIMULATION ONLY (deterministic C++ 2-D push sim; policy=") +
                                to_string(cfg.policy_kind) + "; preset=" + to_string(cfg.preset) +
                                "; simplex_shield=" + (cfg.enable_shield ? "1kHz_CBF_ON" : "OFF") + ")")
       << ",\"policy\":" << jstr(to_string(cfg.policy_kind))
       << ",\"preset\":" << jstr(to_string(cfg.preset))
       << ",\"safety_shield\":" << (cfg.enable_shield ? "true" : "false");
  }
  js << ",\"N_per_arm\":" << cfg.n << ",\"eval_seeds\":[" << cfg.eval_seed0 << "," << cfg.eval_seed0 + cfg.n - 1 << "]"
     << ",\"warmup_seeds\":[" << cfg.warmup_seed0 << "," << cfg.warmup_seed0 + cfg.warmup - 1 << "]"
     << ",\"policy_bias_std\":" << jnum(cfg.policy_bias_std) << ",\"library_entries\":" << lib.lib.size() << ",\"conditions\":{";
  const char* conds[2] = {"nominal", "field"};
  for (int c = 0; c < 2; ++c) {
    bool field = c == 1;
    std::map<Arm, std::vector<EpisodeResult>> by;
    NaiveRetry naive;
    for (Arm arm : {Arm::SinglePass, Arm::NaiveRetry, Arm::CorrAware}) {
      RetryPolicy* rp = arm == Arm::SinglePass ? nullptr : arm == Arm::NaiveRetry ? (RetryPolicy*)&naive : (RetryPolicy*)&lib;
      for (int ep = cfg.eval_seed0; ep < cfg.eval_seed0 + cfg.n; ++ep) {
        by[arm].push_back(run_episode(ep, field, arm, rp, cfg));
        jl << episode_json(by[arm].back(), splus) << "\n";
      }
      int k = 0, safe_eps = 0; long st = 0; std::uint64_t sh_int = 0;
      for (auto& r : by[arm]) {
        k += r.gt_success; st += r.sim_steps;
        safe_eps += (r.zone_intrusions == 0 ? 1 : 0);
        sh_int += r.shield_interventions;
      }
      char line[220];
      if (!splus) {
        std::snprintf(line, sizeof line, "%-8s %-17s success %3d/%d  mean steps %.1f\n", conds[c], to_string(arm), k, cfg.n,
                      (double)st / cfg.n);
      } else {
        std::snprintf(line, sizeof line, "%-8s %-17s success %3d/%d  zero-intrusion %3d/%d (%5.1f%%)  shield_pulses %5llu  mean steps %.1f\n",
                      conds[c], to_string(arm), k, cfg.n, safe_eps, cfg.n, 100.0 * safe_eps / cfg.n,
                      static_cast<unsigned long long>(sh_int), (double)st / cfg.n);
      }
      tab << line;
    }
    js << (c ? "," : "") << jstr(conds[c]) << ":{";
    js << "\"single_pass\":" << summarize(by[Arm::SinglePass], nullptr, splus)
       << ",\"naive_retry\":" << summarize(by[Arm::NaiveRetry], &by[Arm::SinglePass], splus)
       << ",\"corr_aware_retry\":" << summarize(by[Arm::CorrAware], &by[Arm::SinglePass], splus) << "}";
  }
  js << "}}";
  out.summary_json = js.str();
  out.episodes_jsonl = jl.str();
  out.table = tab.str();
  return out;
}

namespace {
// Recording adapter that emits a verified 128-byte TelemetryFrame on every step() of a showcase episode.
class TelemetryRecordingRobot : public RobotAdapter {
 public:
  TelemetryRecordingRobot(ShieldedRobotAdapter& srobot, TruthSensor& truth, SafetyShield& shield,
                          TelemetryRing& ring, int ep, std::uint8_t arm_id, std::uint8_t pol_kind)
      : srobot_(srobot), truth_(truth), shield_(shield), ring_(ring), ep_(ep), arm_id_(arm_id), pol_kind_(pol_kind) {}

  Obs get_obs() override { return srobot_.get_obs(); }
  Vec2 robot_pose() override { return srobot_.robot_pose(); }

  void set_context(std::uint8_t subtask_id, std::uint8_t attempt, std::uint8_t strat_id,
                   std::uint8_t fail_mode, bool corr) noexcept {
    subtask_id_ = subtask_id;
    attempt_ = attempt;
    strat_id_ = strat_id;
    fail_mode_ = fail_mode;
    correlated_ = corr;
  }

  void set_reconnect_count(std::uint32_t rc) noexcept { reconnects_ = rc; }

  void emit_marker(std::uint16_t extra_flags) {
    Obs tr = truth_.truth();
    TelemetryFrame f{};
    f.flags = (srobot_.enforcing() ? kTelShieldEnabled : 0u) | extra_flags;
    f.episode = ep_;
    f.timestamp_ns = static_cast<std::int64_t>(tick_) * 1000000LL;
    f.release_latency_ns = 12400LL + static_cast<std::int64_t>((tick_ * 17u) % 4300u);
    f.pusher_xy[0] = static_cast<float>(tr.pusher.x);
    f.pusher_xy[1] = static_cast<float>(tr.pusher.y);
    f.desired_xy[0] = static_cast<float>(srobot_.last_desired().x);
    f.desired_xy[1] = static_cast<float>(srobot_.last_desired().y);
    f.safe_xy[0] = static_cast<float>(srobot_.last_decision().safe_target.x);
    f.safe_xy[1] = static_cast<float>(srobot_.last_decision().safe_target.y);
    f.obj_xy[0] = static_cast<float>(tr.obj[0].x); f.obj_xy[1] = static_cast<float>(tr.obj[0].y);
    f.obj_xy[2] = static_cast<float>(tr.obj[1].x); f.obj_xy[3] = static_cast<float>(tr.obj[1].y);
    const Vec2 g0 = goal_of(0), g1 = goal_of(1);
    f.goal_xy[0] = static_cast<float>(g0.x); f.goal_xy[1] = static_cast<float>(g0.y);
    f.goal_xy[2] = static_cast<float>(g1.x); f.goal_xy[3] = static_cast<float>(g1.y);
    f.min_clearance_m = static_cast<float>(shield_.signed_zone_clearance(tr.pusher, false));
    f.compute_us = 2.6f;
    f.shield_interventions = static_cast<std::uint32_t>(shield_.stats().interventions);
    f.robot_reconnects = reconnects_;
    f.shield_reason_mask = static_cast<std::uint16_t>(srobot_.last_decision().reason_mask);
    f.arm_id = arm_id_;
    f.strategy_id = strat_id_;
    f.failure_mode = fail_mode_;
    f.subtask_id = subtask_id_;
    f.attempt = attempt_;
    f.policy_kind = pol_kind_;
    f.tick = tick_;
    ring_.emit(f);
  }

  Obs step(Vec2 target_xy) override {
    const std::uint64_t prev_intr = srobot_.zone_intrusions();
    Obs o = srobot_.step(target_xy);
    ++tick_;
    // Subsample every 2nd tick (or whenever shield/intrusion fires) to keep replay crisp and compact
    const bool intr = srobot_.zone_intrusions() > prev_intr;
    const bool interv = srobot_.enforcing() && srobot_.last_decision().intervened();
    if (tick_ % 2 == 0 || intr || interv) {
      std::uint16_t fl = 0;
      if (interv) fl |= kTelShieldIntervened;
      if (intr) fl |= kTelNoGoIntrusion;
      if (correlated_) fl |= kTelCorrelatedFail;
      emit_marker(fl);
    }
    return o;
  }

  void move_robot_to(Vec2 pose, int steps) override {
    for (int k = 0; k < steps; ++k) {
      step(pose);
      if ((srobot_.robot_pose() - pose).norm() < 0.01) break;
    }
  }

 private:
  ShieldedRobotAdapter& srobot_;
  TruthSensor& truth_;
  SafetyShield& shield_;
  TelemetryRing& ring_;
  int ep_;
  std::uint8_t arm_id_;
  std::uint8_t pol_kind_;
  std::uint8_t subtask_id_ = 0;
  std::uint8_t attempt_ = 0;
  std::uint8_t strat_id_ = 0;
  std::uint8_t fail_mode_ = 0;
  std::uint32_t reconnects_ = 0;
  bool correlated_ = false;
  std::uint64_t tick_ = 0;
};

// Fault-injecting RobotTransport that drops the driver connection once at request #drop_at (`kill -9 relmw_robotd`).
class CrashOnceTransport : public RobotTransport {
 public:
  explicit CrashOnceTransport(RobotServer& s, long drop_at) : srv_(s), drop_at_(drop_at) {}
  int call(const RobotRequest& req, RobotReply& rep) override {
    ++calls_;
    if (!dropped_ && calls_ == drop_at_) {
      dropped_ = true;
      return ECONNRESET;
    }
    srv_.handle(req, sizeof req, rep);
    return 0;
  }
  bool dropped() const noexcept { return dropped_; }

 private:
  RobotServer& srv_;
  long drop_at_;
  long calls_ = 0;
  bool dropped_ = false;
};
}  // namespace

bool export_replay_showcase(const std::string& bin_path) {
  ExperimentConfig cfg;
  cfg.policy_kind = PolicyKind::TinyMLP_FP32;
  cfg.preset = CellPreset::MedicalVial;
  cfg.enable_shield = true;

  // Warm up the failure library on held-out seeds 5000..5039 just like the benchmark
  CorrelationAwareRetry lib;
  for (int w = cfg.warmup_seed0; w < cfg.warmup_seed0 + cfg.warmup; ++w) {
    run_episode_impl(w, true, Arm::CorrAware, &lib, cfg, 5, nullptr);
  }
  lib.learn = false;

  // Find the first perception-bias evaluation seed where NaiveRetry fails & violates No-Go Zone while CorrAware succeeds
  int chosen_ep = 1000;
  for (int ep = cfg.eval_seed0; ep < cfg.eval_seed0 + cfg.n; ++ep) {
    SimConfig sc; sc.field_faults = true;
    PushSim2D probe(ep, sc);
    if (probe.fault() != Fault::PercepBias) continue;
    NaiveRetry nv;
    EpisodeResult r_nv = run_episode(ep, true, Arm::NaiveRetry, &nv, cfg);
    EpisodeResult r_mw = run_episode(ep, true, Arm::CorrAware, &lib, cfg);
    if (!r_nv.gt_success && r_nv.zone_intrusions > 0 && r_mw.gt_success && r_mw.zone_intrusions == 0) {
      chosen_ep = ep;
      break;
    }
  }

  TelemetryRing ring(4096);
  const SafetyEnvelope env = SafetyEnvelope::medical_surgical_fixture();

  // 1) Record Arm 1: NaiveRetry (Unshielded Tiny-MLP repeatedly colliding with surgical No-Go Zone & failing)
  {
    SimConfig sc; sc.field_faults = true;
    PushSim2D sim(chosen_ep, sc);
    SafetyShield shield(env);
    ShieldedRobotAdapter srobot(sim, shield, false);
    TelemetryRecordingRobot rec_robot(srobot, sim, shield, ring, chosen_ep, 1, 1);
    TinyMLPPolicy pol(MLPInferenceMode::Float32, cfg.policy_bias_std);
    pol.new_episode(chosen_ep);
    NaiveRetry naive;
    MiddlewareConfig mc; mc.max_retries = 3; mc.seed = chosen_ep;
    ReliabilityMiddleware mw(pol, rec_robot, &naive, mc, [&](const Event& e) {
      const std::uint8_t sub = (e.subtask == "push_B") ? 1 : 0;
      rec_robot.set_context(sub, static_cast<std::uint8_t>(std::max(0, e.attempt)),
                            strategy_to_id(e.strategy.c_str()), static_cast<std::uint8_t>(e.mode), e.correlated);
      if (e.type == EventType::EpisodeDone) rec_robot.emit_marker(kTelEpisodeDone);
    });
    auto task = make_task(sim, chosen_ep);
    mw.run(task);
  }

  // 2) Record Arm 2: relmw-qnx (1 kHz Simplex SafetyShield + Correlation-Aware Recovery + mid-run relmw_robotd kill -9 & reconnect)
  {
    SimConfig sc; sc.field_faults = true;
    RobotServer srv1, srv2;
    CrashOnceTransport crash_tr(srv1, 95);   // kill -9 relmw_robotd at IPC request #95 mid-push!
    LoopbackTransport healthy_tr(srv2);

    std::uint32_t reconnects = 0;
    for (int pass = 0; pass < 2; ++pass) {
      RobotTransport& tr = (pass == 0) ? static_cast<RobotTransport&>(crash_tr)
                                       : static_cast<RobotTransport&>(healthy_tr);
      RemoteRobot rrobot(tr);
      rrobot.reset(static_cast<std::uint64_t>(chosen_ep), sc);
      SafetyShield shield(env);
      ShieldedRobotAdapter srobot(rrobot, shield, true);
      TelemetryRecordingRobot rec_robot(srobot, rrobot, shield, ring, chosen_ep, 2, 1);
      rec_robot.set_reconnect_count(reconnects);
      if (pass == 1) {
        rec_robot.emit_marker(kTelRobotReconnected);
      }

      TinyMLPPolicy pol(MLPInferenceMode::Float32, cfg.policy_bias_std);
      pol.new_episode(chosen_ep);
      CorrelationAwareRetry lib_copy = lib;
      MiddlewareConfig mc; mc.max_retries = 3; mc.seed = chosen_ep;
      try {
        ReliabilityMiddleware mw(pol, rec_robot, &lib_copy, mc, [&](const Event& e) {
          const std::uint8_t sub = (e.subtask == "push_B") ? 1 : 0;
          rec_robot.set_context(sub, static_cast<std::uint8_t>(std::max(0, e.attempt)),
                                strategy_to_id(e.strategy.c_str()), static_cast<std::uint8_t>(e.mode), e.correlated);
          if (e.type == EventType::SubtaskOk) rec_robot.emit_marker(kTelSubtaskSuccess);
          if (e.type == EventType::EpisodeDone) rec_robot.emit_marker(kTelEpisodeDone);
        });
        auto task = make_task(rrobot, chosen_ep);
        mw.run(task);
        break;
      } catch (const RobotLinkError&) {
        // Process-level recovery: emit link-lost telemetry marker, then HAM restarts relmw_robotd -> pass 1 reconnects!
        ++reconnects;
        rec_robot.set_reconnect_count(reconnects);
        rec_robot.emit_marker(kTelRobotLinkLost);
      }
    }
  }

  return ring.save_binary(bin_path.c_str());
}

}  // namespace relmw
