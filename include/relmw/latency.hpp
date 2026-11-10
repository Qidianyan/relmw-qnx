// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Latency / jitter statistics for the real-time control loop (portable, allocation-free record()).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace relmw {

class LatencyStats {
 public:
  // Fixed log-linear histogram: 1 ns .. ~68 s, ~3% relative bucket width; record() is O(1), no allocation.
  LatencyStats();
  void record(std::int64_t ns);           // negative values are clamped to 0
  void reset();
  std::uint64_t count() const { return count_; }
  std::int64_t min() const { return count_ ? min_ : 0; }
  std::int64_t max() const { return count_ ? max_ : 0; }
  double mean() const { return count_ ? static_cast<double>(sum_) / count_ : 0.0; }
  // Quantile estimate from the histogram (q in [0,1]); returns the bucket's upper bound, clamped to max().
  std::int64_t quantile(double q) const;
  // Samples strictly greater than the deadline are counted as misses (0 = no deadline). Resets the miss count.
  void set_deadline(std::int64_t threshold_ns) { thr_ = threshold_ns; over_thr_ = 0; }
  std::uint64_t deadline_misses() const { return over_thr_; }
  // {"count":..,"min_ns":..,"mean_ns":..,"p50_ns":..,"p99_ns":..,"p999_ns":..,"max_ns":..,"deadline_ns":..,"deadline_misses":..}
  std::string to_json() const;

  static int bucket_of(std::int64_t ns);
  static std::int64_t bucket_upper(int b);
  static constexpr int kBuckets = 1024;
 private:
  std::vector<std::uint64_t> h_;
  std::uint64_t count_ = 0;
  std::int64_t min_ = 0, max_ = 0;
  long double sum_ = 0;
  std::int64_t thr_ = 0;
  std::uint64_t over_thr_ = 0;
};

}  // namespace relmw
