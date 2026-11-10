// Small-sample statistics for the paired reliability benchmark.
// C++ port of reliability_mw/stats.py (Newcombe 1998 intervals, exact McNemar, bootstrap).
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace relmw::stats {

struct Interval { double lo, hi; };
struct PairedDiff { double diff, lo, hi; };
struct MeanCI { double mean, lo, hi; };

// 95% Wilson score interval for k/n. n==0 -> {0,0}. Throws std::invalid_argument if k<0, n<0 or k>n.
Interval wilson(int k, int n, double z = 1.96);

// CI for p2 - p1 (independent proportions), Newcombe hybrid score (method 10).
Interval newcombe_diff(int k1, int n1, int k2, int n2, double z = 1.96);

// Paired binary outcomes (a_i, b_i); diff = mean(b) - mean(a); Newcombe paired method 10
// with phi correction. Empty -> {0,0,0}. Bounds clamped to [-1,1].
PairedDiff paired_diff_ci(const std::vector<std::pair<bool, bool>>& pairs, double z = 1.96);

// Exact two-sided McNemar p-value: n=b_only+a_only, p=min(1, 2*sum_{i<=min} C(n,i) 0.5^n). n==0 -> 1.
// Must stay accurate for n up to at least 2000 (compute in log space).
double mcnemar_exact(int b_only, int a_only);

// Percentile bootstrap CI of the mean, deterministic for a given seed. Empty -> {0,0,0}.
MeanCI bootstrap_mean_ci(const std::vector<double>& xs, int n_boot = 2000, double alpha = 0.05,
                         std::uint64_t seed = 0);

}  // namespace relmw::stats
