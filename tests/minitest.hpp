// SPDX-License-Identifier: MIT
// Copyright (c) 2026 elidot
// Minimal dependency-free test harness (so the test suite also builds with q++ for QNX).
#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace minitest {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
inline int run_all() {
  int failed_cases = 0;
  for (auto& c : registry()) {
    int before = failures();
    c.fn();
    bool ok = failures() == before;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
    if (!ok) ++failed_cases;
  }
  std::printf("%zu cases, %d failed\n", registry().size(), failed_cases);
  return failed_cases == 0 ? 0 : 1;
}
}  // namespace minitest

#define MT_CAT2(a, b) a##b
#define MT_CAT(a, b) MT_CAT2(a, b)
#define TEST(name) \
  static void name(); \
  static minitest::Reg MT_CAT(reg_, name)(#name, name); \
  static void name()
#define CHECK(cond) do { if (!(cond)) { std::printf("  %s:%d CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++minitest::failures(); } } while (0)
#define CHECK_NEAR(a, b, tol) do { double _a = (a), _b = (b); if (!(std::fabs(_a - _b) <= (tol))) { std::printf("  %s:%d CHECK_NEAR failed: %s=%.12g vs %s=%.12g (tol %g)\n", __FILE__, __LINE__, #a, _a, #b, _b, (double)(tol)); ++minitest::failures(); } } while (0)
#define TEST_MAIN() int main() { return minitest::run_all(); }
