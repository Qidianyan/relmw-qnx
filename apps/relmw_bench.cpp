// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// relmw_bench: paired N-episode benchmark (none / naive retry / middleware). Runs on Linux, macOS, and QNX 8.0.
//   relmw_bench [--n 100] [--warmup 40] [--bias 0.15] [--policy scripted|mlp|mlp_q12]
//               [--preset standard|medical_vial] [--shield] [--export-replay FILE] [--out results]
#include "relmw/experiment.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>

int main(int argc, char** argv) {
  relmw::ExperimentConfig cfg;
  std::string out = "results";
  std::string replay_bin;
  for (int i = 1; i < argc; ++i) {
    auto next = [&](const char* f) {
      if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", f); std::exit(2); }
      return argv[++i];
    };
    if (!std::strcmp(argv[i], "--n")) cfg.n = std::atoi(next("--n"));
    else if (!std::strcmp(argv[i], "--warmup")) cfg.warmup = std::atoi(next("--warmup"));
    else if (!std::strcmp(argv[i], "--bias")) cfg.policy_bias_std = std::atof(next("--bias"));
    else if (!std::strcmp(argv[i], "--policy")) {
      const char* p = next("--policy");
      if (!std::strcmp(p, "scripted")) cfg.policy_kind = relmw::PolicyKind::Scripted;
      else if (!std::strcmp(p, "mlp") || !std::strcmp(p, "tiny_mlp_fp32")) cfg.policy_kind = relmw::PolicyKind::TinyMLP_FP32;
      else if (!std::strcmp(p, "mlp_q12") || !std::strcmp(p, "tiny_mlp_q12")) cfg.policy_kind = relmw::PolicyKind::TinyMLP_Q12;
      else { std::fprintf(stderr, "unknown --policy %s (expected scripted|mlp|mlp_q12)\n", p); return 2; }
    } else if (!std::strcmp(argv[i], "--preset")) {
      const char* pr = next("--preset");
      if (!std::strcmp(pr, "standard")) cfg.preset = relmw::CellPreset::Standard;
      else if (!std::strcmp(pr, "medical_vial")) cfg.preset = relmw::CellPreset::MedicalVial;
      else { std::fprintf(stderr, "unknown --preset %s (expected standard|medical_vial)\n", pr); return 2; }
    } else if (!std::strcmp(argv[i], "--shield")) {
      cfg.enable_shield = true;
    } else if (!std::strcmp(argv[i], "--export-replay")) {
      replay_bin = next("--export-replay");
    } else if (!std::strcmp(argv[i], "--out")) {
      out = next("--out");
    } else {
      std::fprintf(stderr, "usage: %s [--n N] [--warmup W] [--bias S] [--policy scripted|mlp|mlp_q12] "
                           "[--preset standard|medical_vial] [--shield] [--export-replay FILE] [--out DIR]\n", argv[0]);
      return 2;
    }
  }
  auto t0 = std::chrono::steady_clock::now();
  relmw::BenchOutput r = relmw::run_benchmark(cfg);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ::mkdir(out.c_str(), 0755);
  std::ofstream(out + "/summary.json") << r.summary_json << "\n";
  std::ofstream(out + "/episodes.jsonl") << r.episodes_jsonl;
  std::ofstream(out + "/failure_library.txt") << r.library_txt;
  std::ofstream(out + "/table.txt") << r.table;
  if (!replay_bin.empty()) {
    if (!relmw::export_replay_showcase(replay_bin)) {
      std::fprintf(stderr, "failed to write binary replay showcase: %s\n", replay_bin.c_str());
      return 1;
    }
  }
  std::printf("%s", r.table.c_str());
  std::printf("wall clock %.2f s -> %s/summary.json\n", secs, out.c_str());
  return 0;
}
