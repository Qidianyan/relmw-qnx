# T2: LatencyStats implementation + tests

Working directory: repository root. C++17, no third-party libraries, no network installs.

## Goal
Implement `src/platform/latency.cpp` for the existing header `include/relmw/latency.hpp` (DO NOT modify the header)
and write `tests/test_latency.cpp` with the harness `tests/minitest.hpp` (DO NOT modify it).

## Semantics
- Log-linear histogram with `kBuckets = 1024` buckets covering 0 ns .. about 68.7 s (2^36 ns):
  values 0..63 ns each get their own bucket (bucket b == value b). For v >= 64: let e = floor(log2(v)) (6..35),
  and m = the next 5 bits below the leading 1 bit (i.e. (v >> (e-5)) & 31); bucket = 64 + (e-6)*32 + m.
  Values >= 2^36 go to the last bucket (1023). Total used buckets: 64 + 30*32 = 1024.
- `bucket_upper(b)`: largest value that maps to bucket b (for b<64: b; otherwise ((32+m+1) << (e-5)) - 1;
  last bucket: INT64_MAX). Must satisfy bucket_of(bucket_upper(b)) == b for every b < 1023 and
  bucket_of(bucket_upper(b)+1) == b+1 for b < 1022.
- `record(ns)`: clamp negatives to 0; update count, min, max, sum, histogram; if thr_ > 0 and ns > thr_ then ++over_thr_.
  No heap allocation inside record() (the vector is sized once in the constructor).
- `reset()`: zero everything except the deadline threshold (misses reset to 0).
- `quantile(q)`: q clamped to [0,1]; if count==0 return 0. rank = ceil(q*count) (min 1); walk buckets to the first bucket
  where the cumulative count >= rank; return min(bucket_upper(b), max()) and also at least min().
- `to_json()`: exactly one line, keys in this order:
  `{"count":C,"min_ns":A,"mean_ns":M,"p50_ns":P50,"p99_ns":P99,"p999_ns":P999,"max_ns":X,"deadline_ns":D,"deadline_misses":K}`
  mean printed with %.1f, all others integers.

## Required tests (tests/test_latency.cpp)
1. empty stats: count 0, quantile(0.5)==0, min/max 0, to_json contains "\"count\":0".
2. bucket round-trip property over all buckets as stated above; bucket_of(0)==0, bucket_of(63)==63, bucket_of(64)==64,
   bucket_of(1LL<<40)==1023, bucket_of(-5) treated like 0 by record (record(-5) -> min()==0).
3. relative bucket width: for v in a sweep 64..1e10 (multiply by 1.37 each step), bucket_upper(bucket_of(v)) >= v and
   (bucket_upper(bucket_of(v)) - v) <= v * 0.032 + 1.
4. record 1..1000 (ns): count 1000, min 1, max 1000, mean 500.5, quantile(0.5) within 3.2% above 500 (>=500, <=516),
   quantile(0.99) in [990, 1022], quantile(1.0)==1000.
5. deadline: set_deadline(100); record 50, 100, 101, 5000 -> deadline_misses()==2; reset() -> misses 0, count 0.
6. to_json key order exactly as specified (check with string find positions increasing).

## Acceptance command (must exit 0, no warnings)
```
zig-c++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itests src/platform/latency.cpp tests/test_latency.cpp -o /tmp/relmw_test_latency && /tmp/relmw_test_latency
```

## Constraints
Only create/modify `src/platform/latency.cpp` and `tests/test_latency.cpp`. Do not weaken tests.

## Final reply
Files changed + last 5 lines of test output.
