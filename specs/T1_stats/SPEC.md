# T1: C++ statistics module + tests

Working directory: the repository root (current directory). C++17, no third-party libraries.

## Goal
Implement `src/core/stats.cpp` for the already-written header `include/relmw/stats.hpp` (DO NOT change the header),
and write `tests/test_stats.cpp` using the harness in `tests/minitest.hpp` (DO NOT change it).
This is a port of the Python reference below; numerical results must match.

## Python reference (authoritative semantics)
- wilson(k,n,z): p=k/n; denom=1+z²/n; center=(p+z²/(2n))/denom; half=z*sqrt(p(1-p)/n + z²/(4n²))/denom;
  lo=clamp01(center-half), hi=clamp01(center+half); if k==0 lo=0; if k==n hi=1. n==0 -> (0,0). k<0||n<0||k>n -> throw std::invalid_argument.
- newcombe_diff(k1,n1,k2,n2): p1=k1/n1 (0 if n1==0), p2 likewise; (l1,u1)=wilson(k1,n1); (l2,u2)=wilson(k2,n2); diff=p2-p1;
  lo = diff - hypot(p2-l2, u1-p1); hi = diff + hypot(u2-p2, p1-l1); clamp both to [-1,1].
- paired_diff_ci(pairs): counts n11,n10,n01,n00 (a,b). n_a=n11+n10, n_b=n11+n01, p_a=n_a/n, p_b=n_b/n, Wilson for both.
  phi_den=sqrt(n_a*(n01+n00)*n_b*(n10+n00)) (compute in double); phi=0 if phi_den==0 else clamp((n11*n00-n10*n01)/phi_den,-1,1).
  diff=(n01-n10)/n;
  var_lo=(u_a-p_a)²+(p_b-l_b)²-2phi(u_a-p_a)(p_b-l_b); var_hi=(p_a-l_a)²+(u_b-p_b)²-2phi(p_a-l_a)(u_b-p_b);
  lo=diff-sqrt(max(0,var_lo)); hi=diff+sqrt(max(0,var_hi)); clamp to [-1,1]. Empty -> (0,0,0).
- mcnemar_exact(b,a): negative -> throw std::invalid_argument; n=b+a; n==0 -> 1.0; tail=sum_{i=0..min(b,a)} C(n,i); p=min(1, 2*tail/2^n).
  Use lgamma / log-sum-exp so n=2000 works without overflow.
- bootstrap_mean_ci(xs, n_boot, alpha, seed): mean = sum/n; draw n_boot resamples with replacement using std::mt19937_64(seed)
  and std::uniform_int_distribution<size_t>; sort the resample means; lo/hi = linear-interpolated quantiles at alpha/2 and 1-alpha/2
  (pos=q*(m-1), interpolate between floor and ceil). n_boot<1 or alpha not in (0,1) -> throw std::invalid_argument.

## Required test values (tolerance 1e-9 unless noted) — put ALL of these in tests/test_stats.cpp
- wilson(95,100) = (0.8882480347279118, 0.9784566385436864)
- wilson(0,10) = (0.0, 0.2775401687666166); wilson(10,10) = (0.7224598312333834, 1.0); wilson(0,0) = (0,0)
- wilson(11,10) throws std::invalid_argument
- newcombe_diff(75,100,95,100) = (0.10319675854447863, 0.29730069210659577)
- paired_diff_ci with 74×(T,T), 1×(T,F), 21×(F,T), 4×(F,F) = (0.2, 0.1182201456013012, 0.28901858330165153)
- mcnemar_exact(21,1) = 1.0967254638671875e-05 (relative tol 1e-9); mcnemar_exact(0,0)=1; mcnemar_exact(5,5)=1
- mcnemar_exact(1000,1000) == 1 (within 1e-9) and mcnemar_exact(0,2000) is > 0 and < 1e-300 or exactly 0 (no NaN/inf)
- bootstrap_mean_ci({1..10}, 2000, 0.05, 0): mean == 5.5 exactly, lo < 5.5 < hi, lo > 3, hi < 8;
  calling it twice with same seed gives identical lo/hi; empty input -> (0,0,0).
Add a few extra edge-case tests of your own.

## Build & acceptance command (must pass, exit code 0)
```
zig-c++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itests src/core/stats.cpp tests/test_stats.cpp -o /tmp/relmw_test_stats && /tmp/relmw_test_stats
```
(`zig-c++` is on PATH; it is a clang-based C++ compiler. Do not install anything.)

## Constraints
- Only create/modify: `src/core/stats.cpp`, `tests/test_stats.cpp`. Touch nothing else. No network installs.
- No compiler warnings with -Wall -Wextra.
- Do not weaken or delete any required test.

## Final reply format
List of files changed + the last 5 lines of the test output.
