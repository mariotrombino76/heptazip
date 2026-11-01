# Cross build for Linux ARM64 (aarch64), with the tests run by qemu-user:
#   cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
#   cmake --build build-arm64 -j && ctest --test-dir build-arm64
# Defaults: aarch64-linux-gnu-gcc/g++ (Ubuntu: gcc-aarch64-linux-gnu g++-aarch64-linux-gnu)
# and qemu-aarch64 (qemu-user or qemu-user-static); other compilers with
# -DCMAKE_C_COMPILER=... -DCMAKE_CXX_COMPILER=...
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT CMAKE_C_COMPILER)
  set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
endif()
if(NOT CMAKE_CXX_COMPILER)
  set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
endif()

find_program(SEVENZIP_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static)
if(SEVENZIP_QEMU_AARCH64)
  # libraries of the target: /usr/aarch64-linux-gnu (static binaries do not need them)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${SEVENZIP_QEMU_AARCH64} -L /usr/aarch64-linux-gnu)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
