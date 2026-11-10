// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Deterministic, platform-independent RNG (identical streams on Linux and QNX, any libc).
#pragma once
#include <cstdint>
#include <initializer_list>

namespace relmw {

std::uint64_t splitmix64(std::uint64_t& state);
std::uint64_t hash_mix(std::initializer_list<std::uint64_t> parts);   // stable key -> seed

class Rng {
 public:
  explicit Rng(std::uint64_t seed = 0) { this->seed(seed); }
  void seed(std::uint64_t s);
  std::uint64_t next_u64();
  double uniform();                          // [0,1)
  double uniform(double lo, double hi);
  double normal(double mu = 0, double sigma = 1);
  int randint(int lo, int hi_exclusive);
  int choice_weighted(const double* w, int n);
 private:
  std::uint64_t s_[4];
  bool has_spare_ = false;
  double spare_ = 0;
};

}  // namespace relmw
