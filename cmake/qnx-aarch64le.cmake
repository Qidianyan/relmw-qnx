# CMake toolchain file for QNX SDP 8.0 (aarch64le). Usage:
#   source ~/qnx800/qnxsdp-env.sh
#   cmake -S . -B build-qnx-aarch64le -DCMAKE_TOOLCHAIN_FILE=cmake/qnx-aarch64le.cmake && cmake --build build-qnx-aarch64le
if(NOT DEFINED ENV{QNX_TARGET} OR NOT DEFINED ENV{QNX_HOST})
  message(FATAL_ERROR "QNX_TARGET/QNX_HOST not set: source qnxsdp-env.sh from your QNX SDP 8.0 installation first")
endif()
set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 8.0.0)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(QNX_ARCH_VARIANT gcc_ntoaarch64le)
set(CMAKE_C_COMPILER qcc)
set(CMAKE_C_COMPILER_TARGET ${QNX_ARCH_VARIANT})
set(CMAKE_CXX_COMPILER q++)
set(CMAKE_CXX_COMPILER_TARGET ${QNX_ARCH_VARIANT})
set(CMAKE_SYSROOT $ENV{QNX_TARGET})
set(CMAKE_FIND_ROOT_PATH $ENV{QNX_TARGET} $ENV{QNX_TARGET}/aarch64le)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
