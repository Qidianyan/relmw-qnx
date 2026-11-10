// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
#include "relmw/rng.hpp"
#include <cmath>

namespace relmw {

std::uint64_t splitmix64(std::uint64_t& x) {
  std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

std::uint64_t hash_mix(std::initializer_list<std::uint64_t> parts) {
  std::uint64_t h = 0x243F6A8885A308D3ULL;
  for (auto p : parts) { h ^= p + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2); splitmix64(h); }
  std::uint64_t s = h;
  return splitmix64(s);
}

static inline std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

void Rng::seed(std::uint64_t s) {
  std::uint64_t st = s;
  for (auto& v : s_) v = splitmix64(st);
  has_spare_ = false;
}

std::uint64_t Rng::next_u64() {   // xoshiro256**
  const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
  const std::uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3];
  s_[2] ^= t; s_[3] = rotl(s_[3], 45);
  return result;
}

double Rng::uniform() { return (next_u64() >> 11) * (1.0 / 9007199254740992.0); }
double Rng::uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

double Rng::normal(double mu, double sigma) {
  if (has_spare_) { has_spare_ = false; return mu + sigma * spare_; }
  double u, v, s;
  do { u = uniform(-1, 1); v = uniform(-1, 1); s = u * u + v * v; } while (s >= 1.0 || s == 0.0);
  double m = std::sqrt(-2.0 * std::log(s) / s);
  spare_ = v * m; has_spare_ = true;
  return mu + sigma * u * m;
}

int Rng::randint(int lo, int hi) { return lo + static_cast<int>(next_u64() % static_cast<std::uint64_t>(hi - lo)); }

int Rng::choice_weighted(const double* w, int n) {
  double tot = 0; for (int i = 0; i < n; ++i) tot += w[i];
  double r = uniform() * tot;
  for (int i = 0; i < n; ++i) { if (r < w[i]) return i; r -= w[i]; }
  return n - 1;
}

}  // namespace relmw
