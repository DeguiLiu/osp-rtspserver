# CMake toolchain file for the aarch64 Linux target.
#
# Usage:
#   cmake -S . -B build-arm64 \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain.cmake \
#     -DNEWOSP_DIR=/path/to/newosp
#
# The toolchain expects the Ubuntu cross packages to be installed:
#   sudo apt-get install -y g++-aarch64-linux-gnu gcc-aarch64-linux-gnu \
#                        libc6-dev-arm64-cross linux-libc-dev-arm64-cross
#
# The resulting binaries run under qemu-user:
#   qemu-aarch64 -L /usr/aarch64-linux-gnu ./build-arm64/test_rtsp_parser
# CI wires this into ctest via CMAKE_CROSSCOMPILING_EMULATOR.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Ubuntu ships the cross headers and libraries under /usr/aarch64-linux-gnu.
# Override RS_ARM_SYSROOT when the toolchain lives somewhere else (a
# self-extracted sysroot, for instance).
if(NOT DEFINED RS_ARM_SYSROOT)
  set(RS_ARM_SYSROOT /usr/aarch64-linux-gnu)
endif()
set(CMAKE_FIND_ROOT_PATH ${RS_ARM_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Run the cross-built binaries under qemu-user so ctest can execute them.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64 -L ${RS_ARM_SYSROOT})

set(CMAKE_C_FLAGS_INIT "-O2 -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_INIT "-O2 -g -fno-omit-frame-pointer")
