// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
#include "relmw/retry_policy.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace relmw {

bool is_repeat(const Attempt& a, const std::vector<Attempt>& history) {
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
    const Attempt& h = *it;
    if (h.subtask != a.subtask || h.ok) continue;
    if (h.mode != a.mode) return false;
    if (!a.has_end_obj || !h.has_end_obj) return true;
    return (a.end_obj - h.end_obj).norm() < kRepeatPosTol;
  }
  return false;
}

Choice NaiveRetry::choose(const std::string&, const std::vector<Attempt>&, int) {
  return {make_strategy("replay"), false};
}

namespace {
const std::map<FailureMode, std::vector<std::string>>& prior_table() {
  static const std::map<FailureMode, std::vector<std::string>> t = {
      {FailureMode::StoppedShort, {"obs_lead", "reapproach_left", "replay"}},
      {FailureMode::OffAxisDrift, {"reapproach_left", "reapproach_right"}},
      {FailureMode::LostContact, {"reapproach_left", "replay"}},
  };
  return t;
}
std::string base_name(const std::string& s) { return s.substr(0, s.find('@')); }
bool persistent(const std::string& s) { return s == "relocalize" || s == "slow_mode"; }
}  // namespace

double CorrelationAwareRetry::score(const std::string& subtask, FailureMode mode, const std::string& strat) const {
  auto it = lib.find(subtask + "|" + to_string(mode) + "|" + strat);
  double s = it == lib.end() ? 0 : it->second.first, n = it == lib.end() ? 0 : it->second.second;
  double a0 = 1.0, b0 = 2.0;
  auto pt = prior_table().find(mode);
  if (pt != prior_table().end()) {
    auto f = std::find(pt->second.begin(), pt->second.end(), strat);
    if (f != pt->second.end()) { a0 += 1.5 - 0.5 * (f - pt->second.begin()); b0 = 1.0; }
  }
  return (s + a0) / (n + a0 + b0);
}

Choice CorrelationAwareRetry::choose(const std::string& subtask, const std::vector<Attempt>& history, int) {
  FailureMode mode = FailureMode::None;
  for (auto it = history.rbegin(); it != history.rend(); ++it)
    if (!it->ok) { mode = it->mode; break; }
  std::vector<std::string> used;
  for (auto& h : history) if (!h.ok) used.push_back(h.strategy);
  std::string best; double best_s = -1; int best_rank = 99;
  for (int r = 0; r < 7; ++r) {
    std::string c = kStrategyOrder[r];
    if (std::find(used.begin(), used.end(), c) != used.end()) continue;
    double s = score(subtask, mode, c);
    if (s > best_s || (s == best_s && r < best_rank)) { best = c; best_s = s; best_rank = r; }
  }
  if (best.empty()) return {nullptr, true};
  return {make_strategy(best), score(subtask, mode, "replay") < 0.5};
}

std::unique_ptr<Strategy> CorrelationAwareRetry::proactive(const std::string&) {
  return active_.empty() ? nullptr : make_strategy(active_.back());
}

void CorrelationAwareRetry::record(const std::string& subtask, FailureMode mode, const std::string& strategy, bool ok) {
  std::string b = base_name(strategy);
  if (ok && persistent(b) && std::find(active_.begin(), active_.end(), b) == active_.end()) active_.push_back(b);
  if (!learn || mode == FailureMode::None) return;
  auto& e = lib[subtask + "|" + to_string(mode) + "|" + strategy];
  e.first += ok ? 1 : 0;
  e.second += 1;
}

bool CorrelationAwareRetry::save(const std::string& path) const {
  std::ofstream f(path);
  if (!f) return false;
  for (auto& [k, v] : lib) f << k << ' ' << v.first << ' ' << v.second << '\n';
  return bool(f);
}

bool CorrelationAwareRetry::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) return false;
  lib.clear();
  std::string k; int s, n;
  while (f >> k >> s >> n) lib[k] = {s, n};
  return true;
}

}  // namespace relmw
