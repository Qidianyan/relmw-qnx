// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Tests for src/core/stats.cpp (port of reliability_mw/stats.py).
#include "minitest.hpp"
#include "relmw/stats.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

using relmw::stats::Interval;
using relmw::stats::MeanCI;
using relmw::stats::PairedDiff;

static bool throws_invalid_argument(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

// --- wilson -----------------------------------------------------------------

TEST(wilson_required_95_100) {
  Interval i = relmw::stats::wilson(95, 100);
  CHECK_NEAR(i.lo, 0.8882480347279118, 1e-9);
  CHECK_NEAR(i.hi, 0.9784566385436864, 1e-9);
}

TEST(wilson_required_zero_and_full) {
  Interval lo = relmw::stats::wilson(0, 10);
  CHECK(lo.lo == 0.0);
  CHECK_NEAR(lo.hi, 0.2775401687666166, 1e-9);
  Interval hi = relmw::stats::wilson(10, 10);
  CHECK_NEAR(hi.lo, 0.7224598312333834, 1e-9);
  CHECK(hi.hi == 1.0);
}

TEST(wilson_required_empty) {
  Interval i = relmw::stats::wilson(0, 0);
  CHECK(i.lo == 0.0 && i.hi == 0.0);
}

TEST(wilson_required_invalid_throws) {
  CHECK(throws_invalid_argument([] { relmw::stats::wilson(11, 10); }));
}

TEST(wilson_extra_edges) {
  // Other invalid combinations must throw too.
  CHECK(throws_invalid_argument([] { relmw::stats::wilson(-1, 10); }));
  CHECK(throws_invalid_argument([] { relmw::stats::wilson(5, -10); }));
  CHECK(throws_invalid_argument([] { relmw::stats::wilson(1, 0); }));
  // Custom z widens the interval; k==0 pins lo at 0 and k==n pins hi at 1.
  Interval a = relmw::stats::wilson(0, 10, 2.576);
  CHECK(a.lo == 0.0);
  CHECK(a.hi > 0.2775401687666166);
  Interval b = relmw::stats::wilson(10, 10, 2.576);
  CHECK(b.hi == 1.0);
  CHECK(b.lo < 0.7224598312333834);
  // Symmetric interval around p for k = n/2.
  Interval c = relmw::stats::wilson(50, 100);
  CHECK_NEAR(c.lo + c.hi, 1.0, 1e-12);
  // Bounds stay inside [0,1].
  Interval d = relmw::stats::wilson(0, 1);
  CHECK(d.lo == 0.0 && d.hi >= 0.0 && d.hi <= 1.0);
}

// --- newcombe_diff ----------------------------------------------------------

TEST(newcombe_diff_required) {
  Interval d = relmw::stats::newcombe_diff(75, 100, 95, 100);
  CHECK_NEAR(d.lo, 0.10319675854447863, 1e-9);
  CHECK_NEAR(d.hi, 0.29730069210659577, 1e-9);
}

TEST(newcombe_diff_extra_edges) {
  // Extreme proportions still land inside [-1, 1].
  Interval a = relmw::stats::newcombe_diff(0, 10, 10, 10);
  CHECK(a.lo >= -1.0 && a.hi <= 1.0);
  CHECK_NEAR(a.hi, 1.0, 1e-12);
  Interval b = relmw::stats::newcombe_diff(10, 10, 0, 10);
  CHECK_NEAR(b.lo, -1.0, 1e-12);
  // Zero-sized groups are allowed (p treated as 0, degenerate Wilson).
  Interval c = relmw::stats::newcombe_diff(0, 0, 0, 0);
  CHECK(c.lo == 0.0 && c.hi == 0.0);
  // Propagates wilson() validation errors.
  CHECK(throws_invalid_argument([] { relmw::stats::newcombe_diff(11, 10, 5, 10); }));
}

// --- paired_diff_ci ---------------------------------------------------------

TEST(paired_diff_ci_required) {
  std::vector<std::pair<bool, bool>> pairs;
  for (int i = 0; i < 74; ++i) pairs.emplace_back(true, true);
  pairs.emplace_back(true, false);
  for (int i = 0; i < 21; ++i) pairs.emplace_back(false, true);
  for (int i = 0; i < 4; ++i) pairs.emplace_back(false, false);
  PairedDiff r = relmw::stats::paired_diff_ci(pairs);
  CHECK_NEAR(r.diff, 0.2, 1e-9);
  CHECK_NEAR(r.lo, 0.1182201456013012, 1e-9);
  CHECK_NEAR(r.hi, 0.28901858330165153, 1e-9);
}

TEST(paired_diff_ci_extra_edges) {
  // Empty input.
  PairedDiff e = relmw::stats::paired_diff_ci({});
  CHECK(e.diff == 0.0 && e.lo == 0.0 && e.hi == 0.0);
  // All-concordant data: diff exactly 0, interval symmetric and finite.
  PairedDiff a = relmw::stats::paired_diff_ci({std::make_pair(true, true), std::make_pair(true, true),
                                               std::make_pair(true, true), std::make_pair(true, true)});
  CHECK(a.diff == 0.0);
  CHECK(std::isfinite(a.lo) && std::isfinite(a.hi));
  CHECK(a.lo <= 0.0 && a.hi >= 0.0);
  // All-discordant split (only n10/n01): phi denominator degenerates to 0.
  PairedDiff b = relmw::stats::paired_diff_ci({std::make_pair(true, false), std::make_pair(false, true)});
  CHECK(b.diff == 0.0);
  CHECK(std::isfinite(b.lo) && std::isfinite(b.hi));
  CHECK(b.lo >= -1.0 && b.hi <= 1.0);
  // One-sided discordance: diff is +-1 and bounds clamp to [-1, 1].
  PairedDiff c = relmw::stats::paired_diff_ci({std::make_pair(false, true), std::make_pair(false, true),
                                               std::make_pair(false, true)});
  CHECK_NEAR(c.diff, 1.0, 1e-12);
  CHECK_NEAR(c.hi, 1.0, 1e-12);
  PairedDiff d = relmw::stats::paired_diff_ci({std::make_pair(true, false), std::make_pair(true, false)});
  CHECK_NEAR(d.diff, -1.0, 1e-12);
  CHECK_NEAR(d.lo, -1.0, 1e-12);
}

// --- mcnemar_exact ----------------------------------------------------------

TEST(mcnemar_exact_required) {
  double p = relmw::stats::mcnemar_exact(21, 1);
  CHECK(std::fabs(p - 1.0967254638671875e-05) <= 1e-9 * 1.0967254638671875e-05);
  CHECK(relmw::stats::mcnemar_exact(0, 0) == 1.0);
  CHECK(relmw::stats::mcnemar_exact(5, 5) == 1.0);
}

TEST(mcnemar_exact_required_large_n) {
  CHECK(std::fabs(relmw::stats::mcnemar_exact(1000, 1000) - 1.0) <= 1e-9);
  double p = relmw::stats::mcnemar_exact(0, 2000);
  CHECK(std::isfinite(p));
  CHECK(!(p != p));  // not NaN
  CHECK(p == 0.0 || (p > 0.0 && p < 1e-300));
}

TEST(mcnemar_exact_extra_edges) {
  CHECK(throws_invalid_argument([] { relmw::stats::mcnemar_exact(-1, 5); }));
  CHECK(throws_invalid_argument([] { relmw::stats::mcnemar_exact(5, -1); }));
  // n=1, m=0: tail = 1 -> p = 2*1/2 = 1.
  CHECK(relmw::stats::mcnemar_exact(1, 0) == 1.0);
  CHECK(relmw::stats::mcnemar_exact(0, 1) == 1.0);
  // n=5, b=0: tail = 1 -> p = 2/32.
  CHECK_NEAR(relmw::stats::mcnemar_exact(0, 5), 0.0625, 1e-15);
  // Symmetric in its arguments.
  CHECK(relmw::stats::mcnemar_exact(7, 3) == relmw::stats::mcnemar_exact(3, 7));
  // Tiny but strictly positive tail for a moderately imbalanced large n.
  double q = relmw::stats::mcnemar_exact(3, 97);
  CHECK(q > 0.0 && q < 1.0);
}

// --- bootstrap_mean_ci ------------------------------------------------------

TEST(bootstrap_mean_ci_required) {
  std::vector<double> xs;
  for (int i = 1; i <= 10; ++i) xs.push_back(static_cast<double>(i));
  MeanCI r = relmw::stats::bootstrap_mean_ci(xs, 2000, 0.05, 0);
  CHECK(r.mean == 5.5);
  CHECK(r.lo < 5.5 && r.hi > 5.5);
  CHECK(r.lo > 3.0 && r.hi < 8.0);
}

TEST(bootstrap_mean_ci_required_deterministic) {
  std::vector<double> xs = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};
  MeanCI a = relmw::stats::bootstrap_mean_ci(xs, 2000, 0.05, 0);
  MeanCI b = relmw::stats::bootstrap_mean_ci(xs, 2000, 0.05, 0);
  CHECK(a.mean == b.mean);
  CHECK(a.lo == b.lo);
  CHECK(a.hi == b.hi);
}

TEST(bootstrap_mean_ci_required_empty) {
  MeanCI r = relmw::stats::bootstrap_mean_ci({});
  CHECK(r.mean == 0.0 && r.lo == 0.0 && r.hi == 0.0);
}

TEST(bootstrap_mean_ci_extra_edges) {
  std::vector<double> xs = {1.0, 2.0, 3.0};
  // Argument validation.
  CHECK(throws_invalid_argument([] { relmw::stats::bootstrap_mean_ci({1.0, 2.0}, 0, 0.05, 0); }));
  CHECK(throws_invalid_argument([] { relmw::stats::bootstrap_mean_ci({1.0, 2.0}, -3, 0.05, 0); }));
  CHECK(throws_invalid_argument([] { relmw::stats::bootstrap_mean_ci({1.0, 2.0}, 10, 0.0, 0); }));
  CHECK(throws_invalid_argument([] { relmw::stats::bootstrap_mean_ci({1.0, 2.0}, 10, 1.0, 0); }));
  CHECK(throws_invalid_argument([] { relmw::stats::bootstrap_mean_ci({1.0, 2.0}, 10, -0.5, 0); }));
  CHECK(throws_invalid_argument([] { relmw::stats::bootstrap_mean_ci({1.0, 2.0}, 10, 1.5, 0); }));
  // Single-element input: every resample equals that element.
  MeanCI s = relmw::stats::bootstrap_mean_ci({4.2}, 50, 0.05, 7);
  CHECK(s.mean == 4.2 && s.lo == 4.2 && s.hi == 4.2);
  // Single bootstrap draw collapses the interval onto the mean.
  MeanCI one = relmw::stats::bootstrap_mean_ci(xs, 1, 0.05, 123);
  CHECK(one.lo == one.hi);
  CHECK_NEAR(one.mean, 2.0, 1e-12);
  // Different seeds may differ (not required, but deterministic per seed).
  MeanCI d1 = relmw::stats::bootstrap_mean_ci(xs, 100, 0.05, 1);
  MeanCI d2 = relmw::stats::bootstrap_mean_ci(xs, 100, 0.05, 2);
  CHECK(std::isfinite(d1.lo) && std::isfinite(d1.hi));
  CHECK(d1.lo <= d1.mean && d1.hi >= d1.mean);
  (void)d2;
  // A wider alpha must produce a wider interval.
  MeanCI narrow = relmw::stats::bootstrap_mean_ci(xs, 500, 0.5, 0);
  MeanCI wide = relmw::stats::bootstrap_mean_ci(xs, 500, 0.01, 0);
  CHECK((wide.hi - wide.lo) >= (narrow.hi - narrow.lo));
}

TEST_MAIN()
