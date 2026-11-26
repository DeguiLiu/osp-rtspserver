// Negative compile contract: this translation unit MUST NOT compile.
//
// The whole library is built with -fno-rtti, so no dynamic_cast or typeid
// may appear. This TU uses typeid, which must fail to build.
//
// If this file ever compiles, the RTTI-free contract was broken.
//
// SPDX-License-Identifier: MIT
namespace {
struct Base {
  virtual ~Base() = default;
};
struct Derived final : Base {};
}  // namespace

int main() {
  Derived value;
  return (typeid(value) == typeid(Derived)) ? 0 : 1;
}
