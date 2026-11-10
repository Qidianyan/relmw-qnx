// relmw_bench: paired N-episode benchmark (none / naive retry / middleware). Runs on Linux and QNX.
//   relmw_bench [--n 100] [--warmup 40] [--bias 0.15] [--out results]
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
  for (int i = 1; i < argc; ++i) {
    auto next = [&](const char* f) { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", f); std::exit(2); } return argv[++i]; };
    if (!std::strcmp(argv[i], "--n")) cfg.n = std::atoi(next("--n"));
    else if (!std::strcmp(argv[i], "--warmup")) cfg.warmup = std::atoi(next("--warmup"));
    else if (!std::strcmp(argv[i], "--bias")) cfg.policy_bias_std = std::atof(next("--bias"));
    else if (!std::strcmp(argv[i], "--out")) out = next("--out");
    else { std::fprintf(stderr, "usage: %s [--n N] [--warmup W] [--bias S] [--out DIR]\n", argv[0]); return 2; }
  }
  auto t0 = std::chrono::steady_clock::now();
  relmw::BenchOutput r = relmw::run_benchmark(cfg);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  ::mkdir(out.c_str(), 0755);
  std::ofstream(out + "/summary.json") << r.summary_json << "\n";
  std::ofstream(out + "/episodes.jsonl") << r.episodes_jsonl;
  std::ofstream(out + "/failure_library.txt") << r.library_txt;
  std::ofstream(out + "/table.txt") << r.table;
  std::printf("%s", r.table.c_str());
  std::printf("wall clock %.2f s -> %s/summary.json\n", secs, out.c_str());
  return 0;
}
