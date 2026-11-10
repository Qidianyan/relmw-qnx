// Unit tests for the portable, platform-independent RNG.
#include "relmw/rng.hpp"
#include "minitest.hpp"
#include <cmath>
#include <cstdint>
#include <vector>

using namespace relmw;

// Golden stream value: any change to the RNG algorithm or seeding breaks Linux/QNX result parity.
// Measured once on the current implementation; do not "update" without a portability review.
constexpr std::uint64_t kGolden42 = 1546998764402558742ULL;

TEST(rng_same_seed_identical_stream) {
  Rng a(42), b(42);
  for (int i = 0; i < 1000; ++i) CHECK(a.next_u64() == b.next_u64());
}

TEST(rng_different_seeds_differ) {
  Rng a(1), b(2);
  bool differs = false;
  for (int i = 0; i < 1000; ++i) if (a.next_u64() != b.next_u64()) differs = true;
  CHECK(differs);
}

TEST(rng_golden_first_value) {
  Rng r(42);
  CHECK(r.next_u64() == kGolden42);  // SUSPECTED-CHANGE GUARD: parity with QNX builds
}

TEST(rng_uniform_range_and_mean) {
  Rng r(7);
  double sum = 0;
  const int N = 100000;
  for (int i = 0; i < N; ++i) {
    double u = r.uniform();
    CHECK(u >= 0.0 && u < 1.0);
    sum += u;
  }
  double mean = sum / N;
  CHECK(mean >= 0.49 && mean <= 0.51);
}

TEST(rng_normal_mean_and_variance) {
  Rng r(99);
  double sum = 0, sum2 = 0;
  const int N = 100000;
  for (int i = 0; i < N; ++i) {
    double z = r.normal(0, 1);
    sum += z;
    sum2 += z * z;
  }
  double mean = sum / N;
  double var = sum2 / N - mean * mean;
  CHECK(std::fabs(mean) < 0.02);
  CHECK(var >= 0.97 && var <= 1.03);
}

TEST(rng_randint_range_and_coverage) {
  Rng r(123);
  std::vector<int> hits(4, 0);
  for (int i = 0; i < 100000; ++i) {
    int v = r.randint(3, 7);          // [lo, hi) -> {3,4,5,6}
    CHECK(v >= 3 && v < 7);
    ++hits[v - 3];
  }
  for (int h : hits) CHECK(h > 0);
}

TEST(rng_choice_weighted_zero_weights) {
  Rng r(5);
  const double w[3] = {0.0, 1.0, 0.0};
  for (int i = 0; i < 1000; ++i) CHECK(r.choice_weighted(w, 3) == 1);
}

TEST(rng_hash_mix_order_sensitive_and_stable) {
  std::uint64_t a = hash_mix({1, 2});
  std::uint64_t b = hash_mix({2, 1});
  CHECK(a != b);
  CHECK(hash_mix({1, 2}) == a);       // stable across calls (used as a library key hash)
  CHECK(hash_mix({2, 1}) == b);
}

TEST_MAIN()
