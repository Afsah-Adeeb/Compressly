#pragma once
//
// A ~50-line test harness. A real framework would be a dependency to justify in a
// project whose premise is "no libraries doing the real work", and the only features
// needed here are: register a test, assert, report which one failed.
//
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry();

// Registering from a static initialiser is what lets a test file add cases without the
// main() file having to know about them.
struct Registrar {
  Registrar(const char* name, void (*fn)());
};

[[noreturn]] void fail(const char* file, int line, const std::string& message);

int runAll();

}  // namespace testing

#define TEST(name)                                                  \
  static void name();                                               \
  static ::testing::Registrar registrar_##name(#name, name);        \
  static void name()

#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
  } while (false)

#define CHECK_EQ(a, b)                                              \
  do {                                                              \
    const auto va_ = (a);                                           \
    const auto vb_ = (b);                                           \
    if (!(va_ == vb_)) {                                            \
      std::ostringstream os_;                                       \
      os_ << "CHECK_EQ failed: " #a " == " #b "\n    left  = " << va_ \
          << "\n    right = " << vb_;                               \
      ::testing::fail(__FILE__, __LINE__, os_.str());               \
    }                                                               \
  } while (false)

#define CHECK_THROWS(expr, exception_type)                          \
  do {                                                              \
    bool threw_ = false;                                            \
    try {                                                           \
      (void)(expr);                                                 \
    } catch (const exception_type&) {                               \
      threw_ = true;                                                \
    }                                                               \
    if (!threw_)                                                    \
      ::testing::fail(__FILE__, __LINE__, "expected " #exception_type " from: " #expr); \
  } while (false)
