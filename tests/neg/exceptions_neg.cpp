// Negative compile contract: this translation unit MUST NOT compile.
//
// The whole library is built with -fno-exceptions. A TU that uses try/catch
// must therefore fail to build, which proves the flag is actually in effect
// for the library and not merely requested by the build system.
//
// If this file ever compiles, the exception-free contract was broken.
//
// SPDX-License-Identifier: MIT
namespace {
int ThrowAndCatch() {
  try {
    throw 42;
  } catch (int value) {
    return value;
  }
}
}  // namespace

int main() {
  return ThrowAndCatch();
}
