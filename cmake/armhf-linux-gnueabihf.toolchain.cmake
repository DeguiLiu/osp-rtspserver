# CMake toolchain file for 32-bit ARM (armhf) Linux targets.
#
# Usage:
#   cmake -S . -B build-armhf \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/armhf-linux-gnueabihf.toolchain.cmake \
#     -DNEWOSP_DIR=/path/to/newosp
#
# The toolchain expects the Ubuntu cross packages to be installed:
#   sudo apt-get install -y g++-arm-linux-gnueabihf gcc-arm-linux-gnueabihf \
#                        libc6-dev-armhf-cross linux-libc-dev-armhf-cross
#
# The resulting binaries run under qemu-user:
#   qemu-arm -L /usr/arm-linux-gnueabihf ./build-armhf/test_rtsp_parser
#
# This target is the one that catches 64-bit atomic lock-freedom problems:
# on armhf `std::atomic<uint64_t>` is not lock-free unless the compiler can
# emit LDREXD/STREXD, and libatomic would drag in a lock plus a heap
# allocation.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# Ubuntu ships the cross headers and libraries under /usr/arm-linux-gnueabihf.
# Override RS_ARM_SYSROOT when the toolchain lives somewhere else.
if(NOT DEFINED RS_ARM_SYSROOT)
  set(RS_ARM_SYSROOT /usr/arm-linux-gnueabihf)
endif()
set(CMAKE_FIND_ROOT_PATH ${RS_ARM_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Run the cross-built binaries under qemu-user so ctest can execute them.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm -L ${RS_ARM_SYSROOT})

set(CMAKE_C_FLAGS_INIT "-O2 -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_INIT "-O2 -g -fno-omit-frame-pointer")
