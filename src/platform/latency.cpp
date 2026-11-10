// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Log-linear latency histogram for the real-time control loop (see include/relmw/latency.hpp).
// record() is O(1) and allocation-free: the histogram vector is sized once in the constructor.
#include "relmw/latency.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

namespace relmw {

namespace {

constexpr std::int64_t kLinearEnd = 63;             // values 0..63 ns get one bucket each
constexpr std::int64_t kHistogramCeil = 1LL << 36;  // values >= 2^36 ns (~68.7 s) -> last bucket

// floor(log2(v)) for v >= 1 (portable; no builtins, no allocation).
inline int exponent_of(std::int64_t v) {
  int e = 0;
  for (std::int64_t t = v; t > 1; t >>= 1) ++e;
  return e;
}

}  // namespace

LatencyStats::LatencyStats() : h_(kBuckets, 0) {}

int LatencyStats::bucket_of(std::int64_t ns) {
  std::int64_t v = ns < 0 ? 0 : ns;  // negatives clamp to 0
  if (v <= kLinearEnd) return static_cast<int>(v);
  if (v >= kHistogramCeil) return kBuckets - 1;
  const int e = exponent_of(v);  // 6..35
  const int m = static_cast<int>((v >> (e - 5)) & 31);
  return 64 + (e - 6) * 32 + m;
}

std::int64_t LatencyStats::bucket_upper(int b) {
  if (b <= 0) return 0;
  if (b < 64) return b;
  if (b >= kBuckets - 1) return INT64_MAX;  // last bucket is the overflow catch-all
  const int idx = b - 64;
  const int e = 6 + idx / 32;
  const int m = idx % 32;
  return (static_cast<std::int64_t>(32 + m + 1) << (e - 5)) - 1;
}

void LatencyStats::record(std::int64_t ns) {
  if (ns < 0) ns = 0;
  ++h_[static_cast<std::size_t>(bucket_of(ns))];
  ++count_;
  if (count_ == 1) {
    min_ = ns;
    max_ = ns;
  } else {
    if (ns < min_) min_ = ns;
    if (ns > max_) max_ = ns;
  }
  sum_ += static_cast<long double>(ns);
  if (thr_ > 0 && ns > thr_) ++over_thr_;
}

void LatencyStats::reset() {
  std::fill(h_.begin(), h_.end(), static_cast<std::uint64_t>(0));
  count_ = 0;
  min_ = 0;
  max_ = 0;
  sum_ = 0.0L;
  over_thr_ = 0;
}

std::int64_t LatencyStats::quantile(double q) const {
  if (q < 0.0) q = 0.0;
  if (q > 1.0) q = 1.0;
  if (count_ == 0) return 0;
  std::uint64_t rank = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count_)));
  if (rank < 1) rank = 1;
  if (rank > count_) rank = count_;
  std::uint64_t cum = 0;
  for (int b = 0; b < kBuckets; ++b) {
    cum += h_[b];
    if (cum >= rank) {
      const std::int64_t upper = bucket_upper(b);
      std::int64_t v = upper < max_ ? upper : max_;  // clamp to max()
      return v > min_ ? v : min_;                    // never below min()
    }
  }
  return max_;
}

std::string LatencyStats::to_json() const {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
      "{\"count\":%llu,\"min_ns\":%lld,\"mean_ns\":%.1f,\"p50_ns\":%lld,\"p99_ns\":%lld,"
      "\"p999_ns\":%lld,\"max_ns\":%lld,\"deadline_ns\":%lld,\"deadline_misses\":%llu}",
      static_cast<unsigned long long>(count_),
      static_cast<long long>(min()),
      static_cast<double>(mean()),
      static_cast<long long>(quantile(0.5)),
      static_cast<long long>(quantile(0.99)),
      static_cast<long long>(quantile(0.999)),
      static_cast<long long>(max()),
      static_cast<long long>(thr_),
      static_cast<unsigned long long>(over_thr_));
  return std::string(buf);
}

}  // namespace relmw
