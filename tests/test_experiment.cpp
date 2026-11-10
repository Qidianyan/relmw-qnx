// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// End-to-end benchmark tests: determinism and the expected ordering of the three arms.
#include "minitest.hpp"
#include "relmw/experiment.hpp"
#include <chrono>
#include <cstdio>
#include <sstream>

using namespace relmw;

static int parse_success(const std::string& table, const std::string& cond, const std::string& arm) {
  std::istringstream is(table);
  std::string line;
  while (std::getline(is, line)) {
    std::istringstream ls(line);
    std::string c, a, word; int k = -1;
    ls >> c >> a >> word;
    if (c == cond && a == arm) { std::sscanf(line.c_str() + line.find("success") + 7, "%d", &k); return k; }
  }
  return -1;
}

TEST(single_episode_nominal) {
  ExperimentConfig cfg;
  auto r = run_episode(1000, false, Arm::SinglePass, nullptr, cfg);
  CHECK(r.gt_success);
  CHECK(r.attempts.size() == 2);
}

TEST(benchmark_is_deterministic) {
  ExperimentConfig cfg; cfg.n = 20; cfg.warmup = 5;
  auto a = run_benchmark(cfg), b = run_benchmark(cfg);
  CHECK(a.summary_json == b.summary_json);
  CHECK(a.episodes_jsonl == b.episodes_jsonl);
}

TEST(benchmark_arm_ordering_n100) {
  auto t0 = std::chrono::steady_clock::now();
  ExperimentConfig cfg;
  auto o = run_benchmark(cfg);
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  int none = parse_success(o.table, "field", "single_pass");
  int naive = parse_success(o.table, "field", "naive_retry");
  int mw = parse_success(o.table, "field", "corr_aware_retry");
  CHECK(none > 0);
  CHECK(mw >= naive);
  CHECK(naive >= none);
  CHECK(mw - none >= 10);
  CHECK(parse_success(o.table, "nominal", "single_pass") >= 95);
  CHECK(o.summary_json.find("\"vs_single_pass\"") != std::string::npos);
  CHECK(s < 10.0);
  std::printf("  field: none %d / naive %d / mw %d (%.2f s)\n", none, naive, mw, s);
}

TEST_MAIN()
