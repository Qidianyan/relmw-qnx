// Small-sample statistics for the paired reliability benchmark.
// C++ port of reliability_mw/stats.py (Newcombe 1998 intervals, exact McNemar, bootstrap).
#include "relmw/stats.hpp"
#include "relmw/rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace relmw::stats {

namespace {

inline double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
inline double clamp_unit(double x) { return x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x); }

}  // namespace

Interval wilson(int k, int n, double z) {
  if (k < 0 || n < 0 || k > n) {
    throw std::invalid_argument("wilson: require 0 <= k <= n");
  }
  if (n == 0) return {0.0, 0.0};
  const double dn = static_cast<double>(n);
  const double p = static_cast<double>(k) / dn;
  const double z2 = z * z;
  const double denom = 1.0 + z2 / dn;
  const double center = (p + z2 / (2.0 * dn)) / denom;
  const double half = z * std::sqrt(p * (1.0 - p) / dn + z2 / (4.0 * dn * dn)) / denom;
  double lo = clamp01(center - half);
  double hi = clamp01(center + half);
  if (k == 0) lo = 0.0;
  if (k == n) hi = 1.0;
  return {lo, hi};
}

Interval newcombe_diff(int k1, int n1, int k2, int n2, double z) {
  const double p1 = n1 == 0 ? 0.0 : static_cast<double>(k1) / static_cast<double>(n1);
  const double p2 = n2 == 0 ? 0.0 : static_cast<double>(k2) / static_cast<double>(n2);
  const Interval i1 = wilson(k1, n1, z);
  const Interval i2 = wilson(k2, n2, z);
  const double diff = p2 - p1;
  const double lo = diff - std::hypot(p2 - i2.lo, i1.hi - p1);
  const double hi = diff + std::hypot(i2.hi - p2, p1 - i1.lo);
  return {clamp_unit(lo), clamp_unit(hi)};
}

PairedDiff paired_diff_ci(const std::vector<std::pair<bool, bool>>& pairs, double z) {
  if (pairs.empty()) return {0.0, 0.0, 0.0};
  int n11 = 0, n10 = 0, n01 = 0, n00 = 0;
  for (const auto& pr : pairs) {
    if (pr.first && pr.second) {
      ++n11;
    } else if (pr.first) {
      ++n10;
    } else if (pr.second) {
      ++n01;
    } else {
      ++n00;
    }
  }
  const double n = static_cast<double>(n11 + n10 + n01 + n00);
  const double n_a = static_cast<double>(n11 + n10);
  const double n_b = static_cast<double>(n11 + n01);
  const double p_a = n_a / n;
  const double p_b = n_b / n;
  const Interval ia = wilson(n11 + n10, static_cast<int>(n11 + n10 + n01 + n00), z);
  const Interval ib = wilson(n11 + n01, static_cast<int>(n11 + n10 + n01 + n00), z);
  const double l_a = ia.lo, u_a = ia.hi, l_b = ib.lo, u_b = ib.hi;
  const double phi_den = std::sqrt(n_a * static_cast<double>(n01 + n00) * n_b *
                                   static_cast<double>(n10 + n00));
  double phi = 0.0;
  if (phi_den != 0.0) {
    phi = clamp_unit((static_cast<double>(n11) * n00 - static_cast<double>(n10) * n01) / phi_den);
  }
  const double diff = static_cast<double>(n01 - n10) / n;
  const double ua_pa = u_a - p_a;
  const double pb_lb = p_b - l_b;
  const double pa_la = p_a - l_a;
  const double ub_pb = u_b - p_b;
  const double var_lo = ua_pa * ua_pa + pb_lb * pb_lb - 2.0 * phi * ua_pa * pb_lb;
  const double var_hi = pa_la * pa_la + ub_pb * ub_pb - 2.0 * phi * pa_la * ub_pb;
  double lo = diff - std::sqrt(std::max(0.0, var_lo));
  double hi = diff + std::sqrt(std::max(0.0, var_hi));
  return {diff, clamp_unit(lo), clamp_unit(hi)};
}

double mcnemar_exact(int b_only, int a_only) {
  if (b_only < 0 || a_only < 0) {
    throw std::invalid_argument("mcnemar_exact: b_only and a_only must be >= 0");
  }
  const int n = b_only + a_only;
  if (n == 0) return 1.0;
  const int m = std::min(b_only, a_only);
  // tail = sum_{i=0..m} C(n, i); p = min(1, 2*tail/2^n). Computed in log space
  // (lgamma + log-sum-exp) so large n cannot overflow.
  std::vector<double> logs(static_cast<size_t>(m) + 1);
  const double log_fact_n = std::lgamma(static_cast<double>(n) + 1.0);
  for (int i = 0; i <= m; ++i) {
    logs[static_cast<size_t>(i)] = log_fact_n - std::lgamma(static_cast<double>(i) + 1.0) -
                                   std::lgamma(static_cast<double>(n - i) + 1.0);
  }
  const double mx = *std::max_element(logs.begin(), logs.end());
  double sum = 0.0;
  for (double v : logs) sum += std::exp(v - mx);
  const double log_tail = mx + std::log(sum);
  const double log_p = std::log(2.0) + log_tail - static_cast<double>(n) * std::log(2.0);
  if (log_p >= 0.0) return 1.0;
  return std::exp(log_p);
}

MeanCI bootstrap_mean_ci(const std::vector<double>& xs, int n_boot, double alpha,
                         std::uint64_t seed) {
  if (n_boot < 1) {
    throw std::invalid_argument("bootstrap_mean_ci: n_boot must be >= 1");
  }
  if (!(alpha > 0.0 && alpha < 1.0)) {
    throw std::invalid_argument("bootstrap_mean_ci: alpha must be in (0, 1)");
  }
  if (xs.empty()) return {0.0, 0.0, 0.0};
  const size_t n = xs.size();
  const double sum = std::accumulate(xs.begin(), xs.end(), 0.0);
  const double mean = sum / static_cast<double>(n);
  // relmw::Rng (xoshiro256**) instead of <random> distributions: identical resamples on every libc/libstdc++,
  // so bootstrap intervals computed on the QNX target match the Linux host bit-for-bit.
  relmw::Rng rng(seed);
  std::vector<double> means;
  means.reserve(static_cast<size_t>(n_boot));
  for (int b = 0; b < n_boot; ++b) {
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += xs[static_cast<size_t>(rng.next_u64() % n)];
    means.push_back(acc / static_cast<double>(n));
  }
  std::sort(means.begin(), means.end());
  const size_t m = means.size();
  const auto quantile = [&](double q) -> double {
    const double pos = q * static_cast<double>(m - 1);
    const double fl = std::floor(pos);
    const size_t i0 = static_cast<size_t>(fl);
    const size_t i1 = static_cast<size_t>(std::ceil(pos));
    const double frac = pos - fl;
    return means[i0] + frac * (means[i1] - means[i0]);
  };
  return {mean, quantile(alpha / 2.0), quantile(1.0 - alpha / 2.0)};
}

}  // namespace relmw::stats
