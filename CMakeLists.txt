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
# RELMW_QNX_SIM=ON (Linux only): build the QNX platform layer (src/platform/qnx, compiled with __QNX__) against
# compat/qnx_sim, a user-space simulation of the QNX API subset it uses. QNX API simulation on Linux, not QNX:
# it tests logic and backs the codelab; it says nothing about QNX timing, scheduling or security.
option(RELMW_QNX_SIM "Build the QNX layer against the QNX API simulation on Linux (not QNX)" OFF)
if(RELMW_QNX_SIM AND RELMW_QNX)
  message(FATAL_ERROR "RELMW_QNX_SIM is for Linux hosts; a real QNX build uses the QNX toolchain file")
endif()
if(RELMW_QNX OR RELMW_QNX_SIM)
  set(RELMW_QNX_LAYER ON)
else()
  set(RELMW_QNX_LAYER OFF)
endif()
message(STATUS "relmw: target system = ${CMAKE_SYSTEM_NAME} (QNX layer: ${RELMW_QNX_LAYER}, QNX API simulation: ${RELMW_QNX_SIM})")

add_compile_options(-Wall -Wextra)
if(RELMW_QNX_SIM)
  add_compile_definitions(__QNX__ RELMW_QNX_SIM=1)
  find_package(Threads REQUIRED)
  set(QNX_SIM_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/compat/qnx_sim/include ${CMAKE_CURRENT_SOURCE_DIR}/tools/qnx_stubs/include)
  add_library(qnx_sim STATIC compat/qnx_sim/src/qnx_sim.cpp)
  target_include_directories(qnx_sim PRIVATE ${QNX_SIM_INCLUDES})
  target_link_libraries(qnx_sim PUBLIC Threads::Threads)
  foreach(tool qnxsim_io qnxsim_slog2info)
    add_executable(${tool} compat/qnx_sim/tools/${tool}.cpp)
    target_include_directories(${tool} PRIVATE ${QNX_SIM_INCLUDES})
    target_link_libraries(${tool} qnx_sim)
  endforeach()
endif()

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
  src/core/robot_link.cpp
)
target_include_directories(relmw_core PUBLIC include)

# ---- platform layer: POSIX (Linux host) or QNX-native ---------------------
if(RELMW_QNX_LAYER)
  add_library(relmw_platform STATIC
    src/platform/qnx/qnx_timer.cpp
    src/platform/qnx/qnx_log.cpp
    src/platform/qnx/qnx_resmgr.cpp
    src/platform/qnx/qnx_robot_link.cpp
    src/platform/latency.cpp)
  if(RELMW_QNX_SIM)
    # The QNX sources only: the simulated headers are PRIVATE so apps and the portable core never see them.
    target_include_directories(relmw_platform PRIVATE ${QNX_SIM_INCLUDES})
    target_link_libraries(relmw_platform PUBLIC relmw_core qnx_sim)
  else()
    target_link_libraries(relmw_platform PUBLIC relmw_core slog2)
  endif()
else()
  add_library(relmw_platform STATIC
    src/platform/posix/posix_timer.cpp
    src/platform/posix/posix_log.cpp
    src/platform/posix/posix_ctl.cpp
    src/platform/posix/posix_robot_link.cpp
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

# Robot driver process (owns the simulator + ground truth; supervisor talks to it over the robot link).
# POSIX: AF_UNIX SOCK_SEQPACKET socket; QNX: resource manager /dev/robot0 (_IO_MSG + MsgReply, src/platform/qnx/qnx_robot_link.cpp).
add_executable(relmw_robotd apps/relmw_robotd.cpp)
target_link_libraries(relmw_robotd relmw_platform)
install(TARGETS relmw_robotd DESTINATION bin)

if(RELMW_QNX)
  add_executable(relmw_ham_setup apps/relmw_ham_setup.cpp)
  target_link_libraries(relmw_ham_setup ham)
  target_link_libraries(relmw_platform PUBLIC ham)
  install(TARGETS relmw_ham_setup DESTINATION bin)
endif()

install(TARGETS relmw_bench relmw_supervisor DESTINATION bin)

# ---- tests (ctest) ----------------------------------------------------------
enable_testing()
foreach(t test_rng test_stats test_sim2d test_middleware test_retry_policy test_latency test_posix_platform test_experiment test_rt_noalloc test_robot_link test_plan test_affinity)
  if(RELMW_QNX_LAYER AND t STREQUAL "test_posix_platform")
    continue()   # exercises the POSIX control surface (FIFO files); the QNX layer is covered by scripts/target/relmw_target_run.sh
  endif()
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${t}.cpp)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} relmw_platform)
    add_test(NAME ${t} COMMAND ${t})
  endif()
endforeach()

if(RELMW_QNX_SIM)
  add_executable(test_qnx_sim tests/qnx_sim/test_qnx_sim.cpp)
  target_include_directories(test_qnx_sim PRIVATE ${QNX_SIM_INCLUDES})
  target_link_libraries(test_qnx_sim qnx_sim)
  add_test(NAME test_qnx_sim COMMAND test_qnx_sim)
  add_test(NAME test_sim_e2e COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/qnx_sim/test_sim_e2e.sh ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME test_sim_bench_identical COMMAND sh -c "$<TARGET_FILE:relmw_bench> --n 100 --out ${CMAKE_CURRENT_BINARY_DIR}/sim_bench >/dev/null && cmp ${CMAKE_CURRENT_BINARY_DIR}/sim_bench/episodes.jsonl ${CMAKE_CURRENT_SOURCE_DIR}/results/host_linux/episodes.jsonl && cmp ${CMAKE_CURRENT_BINARY_DIR}/sim_bench/summary.json ${CMAKE_CURRENT_SOURCE_DIR}/results/host_linux/summary.json")
elseif(NOT RELMW_QNX)
  add_test(NAME test_supervisor_e2e COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_supervisor_e2e.sh $<TARGET_FILE:relmw_supervisor> ${CMAKE_CURRENT_SOURCE_DIR}/scripts/relmwctl)
  add_test(NAME test_robotd_e2e COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_robotd_e2e.sh $<TARGET_FILE:relmw_robotd> $<TARGET_FILE:relmw_supervisor>)
  # QNX 8.0 layer: -fsyntax-only against transcribed API stubs (+ stub self-test). GCC only (stubs wrap glibc headers).
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_test(NAME test_qnx_syntax COMMAND ${CMAKE_COMMAND} -E env CXX=${CMAKE_CXX_COMPILER} bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/qnx_syntax_check.sh)
  endif()
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME test_dashboard COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_make_dashboard.py)
    add_test(NAME test_llm_assist COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_llm_assist.py)
    add_test(NAME test_secpol_lint COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_secpol_lint.py)
  endif()
endif()
