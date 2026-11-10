cmake_minimum_required(VERSION 3.16)
project(relmw_qnx VERSION 0.1.0 LANGUAGES C CXX)

# Reliability middleware for robot manipulation, ported to C++17 for QNX SDP 8.0.
# The same tree builds on a Linux host (for tests and benchmarks) and for QNX
# (via cmake/qnx-x86_64.cmake or cmake/qnx-aarch64le.cmake, which use qcc/q++).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  set(RELMW_QNX ON)
else()
  set(RELMW_QNX OFF)
endif()
message(STATUS "relmw: target system = ${CMAKE_SYSTEM_NAME} (QNX layer: ${RELMW_QNX})")

add_compile_options(-Wall -Wextra)

# ---- portable core (no OS calls; identical on Linux and QNX) -------------
add_library(relmw_core STATIC
  src/core/rng.cpp
  src/core/sim2d.cpp
  src/core/policy.cpp
  src/core/verifier.cpp
  src/core/strategies.cpp
  src/core/retry_policy.cpp
  src/core/middleware.cpp
  src/core/stats.cpp
  src/core/experiment.cpp
)
target_include_directories(relmw_core PUBLIC include)

# ---- platform layer: POSIX (Linux host) or QNX-native ---------------------
if(RELMW_QNX)
  add_library(relmw_platform STATIC
    src/platform/qnx/qnx_timer.cpp
    src/platform/qnx/qnx_log.cpp
    src/platform/qnx/qnx_resmgr.cpp
    src/platform/latency.cpp)
  target_link_libraries(relmw_platform PUBLIC relmw_core slog2)
else()
  add_library(relmw_platform STATIC
    src/platform/posix/posix_timer.cpp
    src/platform/posix/posix_log.cpp
    src/platform/posix/posix_ctl.cpp
    src/platform/latency.cpp)
  target_link_libraries(relmw_platform PUBLIC relmw_core)
  find_package(Threads REQUIRED)
  target_link_libraries(relmw_platform PUBLIC Threads::Threads)
endif()
target_include_directories(relmw_platform PUBLIC include)

# ---- applications ---------------------------------------------------------
add_executable(relmw_bench apps/relmw_bench.cpp)
target_link_libraries(relmw_bench relmw_core)

add_executable(relmw_supervisor apps/relmw_supervisor.cpp)
target_link_libraries(relmw_supervisor relmw_platform)

install(TARGETS relmw_bench relmw_supervisor DESTINATION bin)

# ---- tests (ctest) ----------------------------------------------------------
enable_testing()
foreach(t test_rng test_stats test_sim2d test_middleware test_retry_policy test_latency test_experiment)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${t}.cpp)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} relmw_platform)
    add_test(NAME ${t} COMMAND ${t})
  endif()
endforeach()
