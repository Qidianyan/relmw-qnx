// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
#include "relmw/experiment.hpp"
#include "relmw/policy.hpp"
#include "relmw/stats.hpp"
#include "relmw/verifier.hpp"
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>

namespace relmw {

const char* to_string(Arm a) {
  switch (a) {
    case Arm::SinglePass: return "single_pass";
    case Arm::NaiveRetry: return "naive_retry";
    case Arm::CorrAware: return "corr_aware_retry";
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

EpisodeResult run_episode(int ep, bool field, Arm arm, RetryPolicy* rp, const ExperimentConfig& cfg, EventSink sink) {
  SimConfig sc; sc.field_faults = field;
  PushSim2D sim(ep, sc);
  ScriptedPushPolicy pol(cfg.policy_bias_std);
  pol.new_episode(ep);
  MiddlewareConfig mc;
  mc.max_retries = arm == Arm::SinglePass ? 0 : 3;
  mc.episode_step_budget = 2000;
  mc.seed = ep;
  ReliabilityMiddleware mw(pol, sim, rp, mc, std::move(sink));
  auto task = make_task(sim, ep);
  EpisodeReport rep = mw.run(task);
  Obs ts = sim.true_state();
  bool gt = true;
  for (int i = 0; i < kNumObjects; ++i) gt = gt && oracle_verdict(ts, i).ok;
  EpisodeResult r;
  r.ep = ep; r.condition = field ? "field" : "nominal"; r.arm = arm; r.fault = sim.fault();
  r.gt_success = gt; r.mw_success = rep.success; r.abort_reason = rep.abort_reason; r.failed_at = rep.failed_at;
  r.sim_steps = sim.ticks(); r.attempts = rep.attempts;
  return r;
}

namespace {

std::string jstr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
  return o + "\"";
}
std::string jnum(double v) { char b[64]; std::snprintf(b, sizeof b, "%.10g", v); return b; }

std::string summarize(const std::vector<EpisodeResult>& rows, const std::vector<EpisodeResult>* base) {
  int n = (int)rows.size(), k = 0, det = 0, rec = 0, retries = 0, retries_ok = 0, retries_corr = 0, esc = 0;
  int tp = 0, fp = 0, fn = 0, tn = 0;
  std::map<std::string, int> modes, strats;
  std::map<std::string, std::pair<int, int>> by_fault;
  std::vector<double> steps;
  for (auto& r : rows) {
    k += r.gt_success;
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

std::string episode_json(const EpisodeResult& r) {
  std::ostringstream o;
  o << "{\"ep\":" << r.ep << ",\"condition\":" << jstr(r.condition) << ",\"arm\":" << jstr(to_string(r.arm))
    << ",\"fault\":" << jstr(to_string(r.fault)) << ",\"gt_success\":" << (r.gt_success ? "true" : "false")
    << ",\"mw_success\":" << (r.mw_success ? "true" : "false") << ",\"abort\":" << jstr(r.abort_reason)
    << ",\"failed_at\":" << jstr(r.failed_at) << ",\"sim_steps\":" << r.sim_steps << ",\"attempts\":[";
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
  CorrelationAwareRetry lib;
  for (int ep = cfg.warmup_seed0; ep < cfg.warmup_seed0 + cfg.warmup; ++ep) run_episode(ep, true, Arm::CorrAware, &lib, cfg);
  lib.learn = false;
  {
    std::ostringstream l;
    for (auto& [k, v] : lib.lib) l << k << ' ' << v.first << ' ' << v.second << '\n';
    out.library_txt = l.str();
  }
  std::ostringstream js, jl, tab;
  js << "{\"label\":\"SIMULATION ONLY (deterministic C++ 2-D push sim; scripted policy; camera-style verifier)\""
     << ",\"N_per_arm\":" << cfg.n << ",\"eval_seeds\":[" << cfg.eval_seed0 << "," << cfg.eval_seed0 + cfg.n - 1 << "]"
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
        jl << episode_json(by[arm].back()) << "\n";
      }
      int k = 0; long st = 0;
      for (auto& r : by[arm]) { k += r.gt_success; st += r.sim_steps; }
      char line[160];
      std::snprintf(line, sizeof line, "%-8s %-17s success %3d/%d  mean steps %.1f\n", conds[c], to_string(arm), k, cfg.n,
                    (double)st / cfg.n);
      tab << line;
    }
    js << (c ? "," : "") << jstr(conds[c]) << ":{";
    js << "\"single_pass\":" << summarize(by[Arm::SinglePass], nullptr)
       << ",\"naive_retry\":" << summarize(by[Arm::NaiveRetry], &by[Arm::SinglePass])
       << ",\"corr_aware_retry\":" << summarize(by[Arm::CorrAware], &by[Arm::SinglePass]) << "}";
  }
  js << "}}";
  out.summary_json = js.str();
  out.episodes_jsonl = jl.str();
  out.table = tab.str();
  return out;
}

}  // namespace relmw
