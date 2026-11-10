// Tests for src/platform/latency.cpp (LatencyStats log-linear histogram).
#include "minitest.hpp"
#include "relmw/latency.hpp"

#include <cstdint>
#include <string>

using relmw::LatencyStats;

TEST(latency_empty_stats) {
  LatencyStats s;
  CHECK(s.count() == 0);
  CHECK(s.quantile(0.5) == 0);
  CHECK(s.quantile(0.0) == 0);
  CHECK(s.quantile(1.0) == 0);
  CHECK(s.min() == 0);
  CHECK(s.max() == 0);
  CHECK(s.mean() == 0.0);
  CHECK(s.deadline_misses() == 0);
  CHECK(s.to_json().find("\"count\":0") != std::string::npos);
}

TEST(latency_bucket_round_trip) {
  CHECK(LatencyStats::bucket_of(0) == 0);
  CHECK(LatencyStats::bucket_of(63) == 63);
  CHECK(LatencyStats::bucket_of(64) == 64);
  CHECK(LatencyStats::bucket_of(1LL << 40) == LatencyStats::kBuckets - 1);
  CHECK(LatencyStats::bucket_of(-5) == 0);
  for (int b = 0; b < LatencyStats::kBuckets - 1; ++b) {
    const std::int64_t u = LatencyStats::bucket_upper(b);
    CHECK(LatencyStats::bucket_of(u) == b);
    if (b < LatencyStats::kBuckets - 2) {
      CHECK(LatencyStats::bucket_of(u + 1) == b + 1);
    }
  }
  CHECK(LatencyStats::bucket_upper(LatencyStats::kBuckets - 1) == INT64_MAX);
  // record() treats negatives like 0.
  LatencyStats s;
  s.record(-5);
  CHECK(s.count() == 1);
  CHECK(s.min() == 0);
  CHECK(s.max() == 0);
}

TEST(latency_relative_bucket_width) {
  for (double v = 64.0; v <= 1e10; v *= 1.37) {
    const std::int64_t x = static_cast<std::int64_t>(v);
    const std::int64_t u = LatencyStats::bucket_upper(LatencyStats::bucket_of(x));
    CHECK(u >= x);
    CHECK(static_cast<double>(u - x) <= static_cast<double>(x) * 0.032 + 1.0);
  }
}

TEST(latency_record_basic) {
  LatencyStats s;
  for (int i = 1; i <= 1000; ++i) s.record(i);
  CHECK(s.count() == 1000);
  CHECK(s.min() == 1);
  CHECK(s.max() == 1000);
  CHECK_NEAR(s.mean(), 500.5, 1e-9);
  const double p50 = static_cast<double>(s.quantile(0.5));
  CHECK(p50 >= 500.0 && p50 <= 516.0);
  const double p99 = static_cast<double>(s.quantile(0.99));
  CHECK(p99 >= 990.0 && p99 <= 1022.0);
  CHECK(s.quantile(1.0) == 1000);
  CHECK(s.quantile(-1.0) >= 1);   // q clamped to [0,1]
  CHECK(s.quantile(2.0) == 1000);
}

TEST(latency_deadline_misses) {
  LatencyStats s;
  s.set_deadline(100);
  s.record(50);
  s.record(100);
  s.record(101);
  s.record(5000);
  CHECK(s.count() == 4);
  CHECK(s.deadline_misses() == 2);  // strictly greater than the deadline
  s.reset();
  CHECK(s.deadline_misses() == 0);
  CHECK(s.count() == 0);
  CHECK(s.min() == 0);
  CHECK(s.max() == 0);
  CHECK(s.mean() == 0.0);
  CHECK(s.quantile(0.5) == 0);
  // reset() keeps the deadline threshold but clears everything else.
  s.record(150);
  CHECK(s.deadline_misses() == 1);
  s.set_deadline(1000);
  CHECK(s.deadline_misses() == 0);  // set_deadline resets the miss count
}

TEST(latency_json_key_order) {
  LatencyStats s;
  s.set_deadline(250);
  for (int i = 1; i <= 100; ++i) s.record(i * 3);
  const std::string j = s.to_json();
  const char* keys[] = {"\"count\":", "\"min_ns\":", "\"mean_ns\":", "\"p50_ns\":", "\"p99_ns\":",
                        "\"p999_ns\":", "\"max_ns\":", "\"deadline_ns\":", "\"deadline_misses\":"};
  std::size_t prev = 0;
  for (const char* k : keys) {
    const std::size_t pos = j.find(k);
    CHECK(pos != std::string::npos);
    CHECK(pos > prev);
    prev = pos;
  }
  CHECK(j.rfind("{\"count\":", 0) == 0);          // starts with the first key
  CHECK(j.find('\n') == std::string::npos);        // exactly one line
  CHECK(j.size() > 0 && j[j.size() - 1] == '}');
}

TEST(latency_json_exact_format) {
  LatencyStats s;
  s.record(0);
  s.record(1);
  s.record(2);
  s.record(3);
  CHECK(s.to_json() ==
        "{\"count\":4,\"min_ns\":0,\"mean_ns\":1.5,\"p50_ns\":1,\"p99_ns\":3,\"p999_ns\":3,"
        "\"max_ns\":3,\"deadline_ns\":0,\"deadline_misses\":0}");
}

TEST_MAIN()
