#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 elidot
# Build + test on a Linux host. Uses $CXX if set, else g++/clang++, else zig-c++.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -z "${CXX:-}" ]; then
  for c in g++ clang++ zig-c++; do command -v $c >/dev/null && { export CXX=$c; break; }; done
fi
if [ -z "${CC:-}" ]; then
  case "$CXX" in zig-c++) export CC=zig-cc;; clang++) export CC=clang;; *) export CC=gcc;; esac
fi
if command -v cmake >/dev/null 2>&1; then
  NJOBS=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu 2>/dev/null || echo 4)
  cmake -S . -B build-host -G "${CMAKE_GENERATOR:-Ninja}" -DCMAKE_BUILD_TYPE=Release
  cmake --build build-host -j"$NJOBS"
  (cd build-host && ctest --output-on-failure)
else
  exec bash scripts/build_direct.sh host
fi
